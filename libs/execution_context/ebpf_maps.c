// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

#include "ebpf_async.h"
#include "ebpf_bitmap.h"
#include "ebpf_epoch.h"
#include "ebpf_handle.h"
#include "ebpf_maps.h"
#include "ebpf_object.h"
#include "ebpf_program.h"

#define PAD_CACHE(X) ((X + EBPF_CACHE_LINE_SIZE - 1) & ~(EBPF_CACHE_LINE_SIZE - 1))

typedef struct _ebpf_core_map
{
    ebpf_object_t object;
    ebpf_utf8_string_t name;
    ebpf_map_definition_in_memory_t ebpf_map_definition;
    ebpf_lock_t lock;
    uint32_t original_value_size;
    struct _ebpf_core_map* inner_map_template;
    // Flag that is set the first time an async operation is queued to the map.
    // This flag only transitions from off -> on. When this flag is set,
    // updates to the map acquire the lock and check the async_contexts list.
    // Note that queueing an async operation thus causes a perf degradation
    // for all subsequent updates, so should only be allowed to admin.
    bool async_contexts_trip_wire;
    LIST_ENTRY async_contexts;
    uint8_t* data;
} ebpf_core_map_t;

typedef struct _ebpf_core_map_async_context
{
    LIST_ENTRY entry;
    ebpf_core_map_t* map;
    void* async_context;
} ebpf_core_map_async_context_t;

typedef struct _ebpf_core_object_map
{
    ebpf_core_map_t core_map;
    bool is_program_type_set;
    ebpf_program_type_t program_type;
} ebpf_core_object_map_t;

typedef struct _ebpf_core_lru_map
{
    ebpf_core_map_t core_map;
    // https://github.com/microsoft/ebpf-for-windows/issues/557
    // Investigate replacing this with a heap to speed up finding oldest key.
    ebpf_hash_table_t* key_history;
} ebpf_core_lru_map_t;

typedef struct _ebpf_core_lpm_map
{
    ebpf_core_map_t core_map;
    uint32_t max_prefix;
    // Bitmap of prefix lengths inserted into the map.
    uint8_t data[1];
} ebpf_core_lpm_map_t;

/**
 * Core map structure for BPF_MAP_TYPE_QUEUE and BPF_MAP_TYPE_STACK
 * ebpf_core_queue_map_t and ebpf_core_stack_map_t store arrays of uint8_t*
 * pointers. Each pointer stores a version of a value that has been pushed to
 * the queue or stack. The structure can't store the map values directly as the
 * caller expects items returned from find_and_delete to remain unmodified. If
 * items are stored directly in the array, then a sequence of:
 * 1) update
 * 2) find_and_delete
 * 3) update
 * 4) find_and_delete
 * can result in aliasing the record, which would result in unexpected behavior.
 */

typedef struct _ebpf_core_queue_map
{
    ebpf_core_map_t core_map;
    size_t start;
    size_t end;
    uint8_t* slots[1];
} ebpf_core_queue_map_t;

typedef struct _ebpf_core_stack_map
{
    ebpf_core_map_t core_map;
    size_t top_of_stack; // Points to the next empty slot.
    uint8_t* slots[1];
} ebpf_core_stack_map_t;

_Ret_notnull_ static const ebpf_program_type_t*
_get_map_program_type(_In_ const ebpf_object_t* object)
{
    const ebpf_core_object_map_t* map = (const ebpf_core_object_map_t*)object;
    return &map->program_type;
}

typedef struct _ebpf_map_function_table
{
    ebpf_core_map_t* (*create_map)(_In_ const ebpf_map_definition_in_memory_t* map_definition);
    void (*delete_map)(_In_ ebpf_core_map_t* map);
    ebpf_result_t (*associate_program)(_In_ ebpf_map_t* map, _In_ const ebpf_program_t* program);
    ebpf_result_t (*find_entry)(
        _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data);
    ebpf_object_t* (*get_object_from_entry)(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key);
    ebpf_result_t (*update_entry)(
        _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ const uint8_t* value, ebpf_map_option_t option);
    ebpf_result_t (*update_entry_with_handle)(
        _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, uintptr_t value_handle, ebpf_map_option_t option);
    ebpf_result_t (*update_entry_per_cpu)(
        _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ const uint8_t* value, ebpf_map_option_t option);
    ebpf_result_t (*delete_entry)(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key);
    ebpf_result_t (*next_key)(_In_ ebpf_core_map_t* map, _In_ const uint8_t* previous_key, _Out_ uint8_t* next_key);
} ebpf_map_function_table_t;

ebpf_map_function_table_t ebpf_map_function_tables[];

const ebpf_map_definition_in_memory_t*
ebpf_map_get_definition(_In_ const ebpf_map_t* map)
{
    return &map->ebpf_map_definition;
}

uint32_t
ebpf_map_get_effective_value_size(_In_ const ebpf_map_t* map)
{
    return map->original_value_size;
}

static ebpf_core_map_t*
_create_array_map_with_map_struct_size(
    size_t map_struct_size, _In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    ebpf_result_t retval;
    size_t map_data_size = 0;
    ebpf_core_map_t* map = NULL;

    retval = ebpf_safe_size_t_multiply(map_definition->max_entries, map_definition->value_size, &map_data_size);
    if (retval != EBPF_SUCCESS) {
        goto Done;
    }

    size_t full_map_size;
    retval = ebpf_safe_size_t_add(PAD_CACHE(map_struct_size), map_data_size, &full_map_size);
    if (retval != EBPF_SUCCESS) {
        goto Done;
    }

    // allocate
    map = ebpf_epoch_allocate(full_map_size);
    if (map == NULL) {
        goto Done;
    }
    memset(map, 0, full_map_size);

    map->ebpf_map_definition = *map_definition;
    map->data = ((uint8_t*)map) + PAD_CACHE(map_struct_size);

Done:
    return map;
}

static ebpf_core_map_t*
_create_array_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    return _create_array_map_with_map_struct_size(sizeof(ebpf_core_map_t), map_definition);
}

static void
_delete_array_map(_In_ ebpf_core_map_t* map)
{
    ebpf_epoch_free(map);
}

static ebpf_result_t
_find_array_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data)
{
    uint32_t key_value;
    if (!map || !key || delete_on_success)
        return EBPF_INVALID_ARGUMENT;

    key_value = *(uint32_t*)key;

    if (key_value >= map->ebpf_map_definition.max_entries)
        return EBPF_INVALID_ARGUMENT;

    *data = &map->data[key_value * map->ebpf_map_definition.value_size];
    return EBPF_SUCCESS;
}

static ebpf_result_t
_update_array_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_opt_ const uint8_t* data, ebpf_map_option_t option)
{
    uint32_t key_value;

    if (!map || !key || (option == EBPF_NOEXIST))
        return EBPF_INVALID_ARGUMENT;

    key_value = *(uint32_t*)key;

    if (key_value >= map->ebpf_map_definition.max_entries)
        return EBPF_INVALID_ARGUMENT;

    uint8_t* entry = &map->data[*key * map->ebpf_map_definition.value_size];
    if (data) {
        memcpy(entry, data, map->ebpf_map_definition.value_size);
    } else {
        memset(entry, 0, map->ebpf_map_definition.value_size);
    }
    return EBPF_SUCCESS;
}

static ebpf_result_t
_delete_array_map_entry_with_reference(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, ebpf_object_type_t value_type)
{
    uint32_t key_value;
    if (!map || !key)
        return EBPF_INVALID_ARGUMENT;

    key_value = *(uint32_t*)key;

    if (key_value >= map->ebpf_map_definition.max_entries)
        return EBPF_INVALID_ARGUMENT;

    uint8_t* entry = &map->data[key_value * map->ebpf_map_definition.value_size];
    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);
    if (value_type != EBPF_OBJECT_UNKNOWN) {
        ebpf_id_t id = *(ebpf_id_t*)entry;
        ebpf_object_dereference_by_id(id, value_type);
    }
    memset(entry, 0, map->ebpf_map_definition.value_size);
    ebpf_lock_unlock(&map->lock, lock_state);
    return EBPF_SUCCESS;
}

static ebpf_result_t
_delete_array_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    return _delete_array_map_entry_with_reference(map, key, EBPF_OBJECT_UNKNOWN);
}

static ebpf_result_t
_next_array_map_key(_In_ ebpf_core_map_t* map, _In_ const uint8_t* previous_key, _Out_ uint8_t* next_key)
{
    uint32_t key_value;
    if (!map || !next_key)
        return EBPF_INVALID_ARGUMENT;

    if (previous_key) {
        key_value = *(uint32_t*)previous_key;
        key_value++;
    } else
        key_value = 0;

    if (key_value >= map->ebpf_map_definition.max_entries)
        return EBPF_NO_MORE_KEYS;

    *(uint32_t*)next_key = key_value;

    return EBPF_SUCCESS;
}

static ebpf_core_map_t*
_create_object_array_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    return _create_array_map_with_map_struct_size(sizeof(ebpf_core_object_map_t), map_definition);
}

static void
_delete_object_array_map(_In_ ebpf_core_map_t* map, ebpf_object_type_t value_type)
{
    // Release all entry references.
    for (uint32_t i = 0; i < map->ebpf_map_definition.max_entries; i++) {
        ebpf_id_t id = *(ebpf_id_t*)&map->data[i * map->ebpf_map_definition.value_size];
        ebpf_object_dereference_by_id(id, value_type);
    }

    _delete_array_map(map);
}

static void
_delete_program_array_map(_In_ ebpf_core_map_t* map)
{
    _delete_object_array_map(map, EBPF_OBJECT_PROGRAM);
}

static void
_delete_map_array_map(_In_ ebpf_core_map_t* map)
{
    _delete_object_array_map(map, EBPF_OBJECT_MAP);
}

static ebpf_result_t
_associate_program_with_prog_array_map(_In_ ebpf_core_map_t* map, _In_ const ebpf_program_t* program)
{
    ebpf_assert(map->ebpf_map_definition.type == BPF_MAP_TYPE_PROG_ARRAY);
    ebpf_core_object_map_t* program_array = (ebpf_core_object_map_t*)map;

    // Validate that the program type is
    // not in conflict with the map's program type.
    const ebpf_program_type_t* program_type = ebpf_program_type(program);
    ebpf_result_t result = EBPF_SUCCESS;

    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);

    if (!program_array->is_program_type_set) {
        program_array->is_program_type_set = TRUE;
        program_array->program_type = *program_type;
    } else if (memcmp(&program_array->program_type, program_type, sizeof(*program_type)) != 0) {
        result = EBPF_INVALID_FD;
    }

    ebpf_lock_unlock(&map->lock, lock_state);

    return result;
}

static bool // Returns true if ok, false if not.
_check_value_type(_In_ const ebpf_core_map_t* outer_map, _In_ const ebpf_object_t* value_object)
{
    if (outer_map->ebpf_map_definition.type != BPF_MAP_TYPE_ARRAY_OF_MAPS &&
        outer_map->ebpf_map_definition.type != BPF_MAP_TYPE_HASH_OF_MAPS) {
        return true;
    }

    ebpf_core_map_t* template = outer_map->inner_map_template;
    const ebpf_map_t* value_map = (ebpf_map_t*)value_object;

    bool allowed = (template != NULL) && (value_map->ebpf_map_definition.type == template->ebpf_map_definition.type) &&
                   (value_map->ebpf_map_definition.key_size == template->ebpf_map_definition.key_size) &&
                   (value_map->ebpf_map_definition.value_size == template->ebpf_map_definition.value_size) &&
                   (value_map->ebpf_map_definition.max_entries == template->ebpf_map_definition.max_entries);

    return allowed;
}

// Validate that a value handle is appropriate for this map,
// and if so, return a pointer to the object with that handle.
static ebpf_result_t
_get_map_value_object(
    _In_ const ebpf_core_map_t* map,
    ebpf_handle_t value_handle,
    ebpf_object_type_t value_type,
    _Outptr_ ebpf_object_t** value_object_result)
{
    // Convert value handle to an object pointer.
    ebpf_object_t* value_object = NULL;
    ebpf_result_t result = ebpf_reference_object_by_handle(value_handle, value_type, &value_object);
    if (result != EBPF_SUCCESS)
        return result;

    const ebpf_program_type_t* value_program_type =
        (value_object->get_program_type) ? value_object->get_program_type(value_object) : NULL;

    if (value_type == EBPF_OBJECT_MAP) {
        // Validate that the value is of the correct type.
        if (!_check_value_type(map, value_object)) {
            result = EBPF_INVALID_OBJECT;
            goto Error;
        }
    }

    // Validate that the value's program type (if any) is
    // not in conflict with the map's program type.
    if (value_program_type != NULL) {
        ebpf_core_object_map_t* map_of_objects = (ebpf_core_object_map_t*)map;
        if (!map_of_objects->is_program_type_set) {
            map_of_objects->is_program_type_set = TRUE;
            map_of_objects->program_type = *value_program_type;
        } else if (memcmp(&map_of_objects->program_type, value_program_type, sizeof(*value_program_type)) != 0) {
            result = EBPF_INVALID_FD;
            goto Error;
        }
    }

    *value_object_result = value_object;
    return EBPF_SUCCESS;

Error:
    ebpf_object_release_reference((ebpf_object_t*)value_object);
    return result;
}

static ebpf_result_t
_update_array_map_entry_with_handle(
    _In_ ebpf_core_map_t* map,
    _In_ const uint8_t* key,
    ebpf_object_type_t value_type,
    uintptr_t value_handle,
    ebpf_map_option_t option)
{
    if (!map || !key || (option == EBPF_NOEXIST))
        return EBPF_INVALID_ARGUMENT;

    uint32_t index = *(uint32_t*)key;

    if (index >= map->ebpf_map_definition.max_entries)
        return EBPF_INVALID_ARGUMENT;

    ebpf_result_t result = EBPF_SUCCESS;

    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);

    ebpf_object_t* value_object = NULL;
    result = _get_map_value_object(map, value_handle, value_type, &value_object);
    if (result != EBPF_SUCCESS) {
        goto Done;
    }

    // Release the reference on the old ID stored here, if any.
    uint8_t* entry = &map->data[*key * map->ebpf_map_definition.value_size];
    ebpf_id_t old_id = *(ebpf_id_t*)entry;
    if (old_id) {
        ebpf_object_dereference_by_id(old_id, value_type);
    }

    // Store the object ID as the value.
    memcpy(entry, &value_object->id, map->ebpf_map_definition.value_size);

Done:
    ebpf_lock_unlock(&map->lock, lock_state);

    return result;
}

static ebpf_result_t
_update_prog_array_map_entry_with_handle(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, uintptr_t value_handle, ebpf_map_option_t option)
{
    return _update_array_map_entry_with_handle(map, key, EBPF_OBJECT_PROGRAM, value_handle, option);
}

static ebpf_result_t
_update_map_array_map_entry_with_handle(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, uintptr_t value_handle, ebpf_map_option_t option)
{
    return _update_array_map_entry_with_handle(map, key, EBPF_OBJECT_MAP, value_handle, option);
}

static ebpf_result_t
_delete_program_array_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    return _delete_array_map_entry_with_reference(map, key, EBPF_OBJECT_PROGRAM);
}

static ebpf_result_t
_delete_map_array_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    return _delete_array_map_entry_with_reference(map, key, EBPF_OBJECT_MAP);
}

/**
 * @brief Get an object from a map entry that holds objects, such
 * as a program array or array of maps.  The object returned holds a
 * reference that the caller is responsible for releasing.
 *
 * @param[in] map Array map to search.
 * @param[in] key Pointer to the key to search for.
 * @returns Object pointer, or NULL if none.
 */
static _Ret_maybenull_ ebpf_object_t*
_get_object_from_array_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    uint32_t index = *(uint32_t*)key;

    // We need to take a lock here to make sure we can
    // safely reference the object when another thread
    // might be trying to delete the entry we find.
    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);

    ebpf_object_t* object = NULL;
    uint8_t* value = NULL;
    if (_find_array_map_entry(map, (uint8_t*)&index, false, &value) == EBPF_SUCCESS) {
        ebpf_id_t id = *(ebpf_id_t*)&map->data[index * map->ebpf_map_definition.value_size];
        ebpf_object_type_t value_type =
            (map->ebpf_map_definition.type == BPF_MAP_TYPE_PROG_ARRAY) ? EBPF_OBJECT_PROGRAM : EBPF_OBJECT_MAP;
        (void)ebpf_object_reference_by_id(id, value_type, &object);
    }

    ebpf_lock_unlock(&map->lock, lock_state);

    return object;
}

static ebpf_core_map_t*
_create_hash_map_internal(
    size_t map_struct_size,
    _In_ const ebpf_map_definition_in_memory_t* map_definition,
    _In_opt_ void (*extract_function)(
        _In_ const uint8_t* value, _Outptr_ const uint8_t** data, _Out_ size_t* length_in_bits))
{
    ebpf_result_t retval;
    ebpf_core_map_t* map = NULL;

    map = ebpf_epoch_allocate(map_struct_size);
    if (map == NULL) {
        retval = EBPF_NO_MEMORY;
        goto Done;
    }

    map->ebpf_map_definition = *map_definition;
    map->data = NULL;

    // Note:
    // ebpf_hash_table_t doesn't require synchronization as long as allocations
    // are performed using the epoch allocator.
    retval = ebpf_hash_table_create(
        (ebpf_hash_table_t**)&map->data,
        ebpf_epoch_allocate,
        ebpf_epoch_free,
        map->ebpf_map_definition.key_size,
        map->ebpf_map_definition.value_size,
        map->ebpf_map_definition.max_entries,
        extract_function);
    if (retval != EBPF_SUCCESS) {
        goto Done;
    }

    retval = EBPF_SUCCESS;

Done:
    if (retval != EBPF_SUCCESS) {
        if (map && map->data) {
            ebpf_hash_table_destroy((ebpf_hash_table_t*)map->data);
        }
        ebpf_epoch_free(map);
        map = NULL;
    }
    return map;
}

static ebpf_core_map_t*
_create_hash_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    return _create_hash_map_internal(sizeof(ebpf_core_map_t), map_definition, NULL);
}

static ebpf_core_map_t*
_create_object_hash_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    return _create_hash_map_internal(sizeof(ebpf_core_object_map_t), map_definition, NULL);
}

static ebpf_core_map_t*
_create_lru_hash_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    ebpf_core_lru_map_t* map =
        (ebpf_core_lru_map_t*)_create_hash_map_internal(sizeof(ebpf_core_lru_map_t), map_definition, NULL);
    if (map) {
        ebpf_result_t retval;
        // Note:
        // ebpf_hash_table_t doesn't require synchronization as long as allocations
        // are performed using the epoch allocator.
        retval = ebpf_hash_table_create(
            &map->key_history,
            ebpf_epoch_allocate,
            ebpf_epoch_free,
            map->core_map.ebpf_map_definition.key_size,
            sizeof(uint64_t),
            map->core_map.ebpf_map_definition.max_entries,
            NULL);
        if (retval != EBPF_SUCCESS) {
            ebpf_hash_table_destroy((ebpf_hash_table_t*)map->core_map.data);
            ebpf_epoch_free(map);
            map = NULL;
        }
    }

    return (map) ? &map->core_map : NULL;
}

static void
_delete_hash_map(_In_ ebpf_core_map_t* map)
{
    ebpf_hash_table_destroy((ebpf_hash_table_t*)map->data);
    ebpf_epoch_free(map);
}

static void
_delete_lru_hash_map(_In_ ebpf_core_map_t* map)
{
    ebpf_core_lru_map_t* lru_map = EBPF_FROM_FIELD(ebpf_core_lru_map_t, core_map, map);
    ebpf_hash_table_destroy(lru_map->key_history);
    ebpf_hash_table_destroy((ebpf_hash_table_t*)lru_map->core_map.data);
    ebpf_epoch_free(map);
}

static void
_delete_object_hash_map(_In_ ebpf_core_map_t* map)
{
    // Release all entry references.
    uint8_t* next_key;
    for (uint8_t* previous_key = NULL;; previous_key = next_key) {
        uint8_t* value;
        ebpf_result_t result =
            ebpf_hash_table_next_key_pointer_and_value((ebpf_hash_table_t*)map->data, previous_key, &next_key, &value);
        if (result != EBPF_SUCCESS) {
            break;
        }
        ebpf_id_t id = *(ebpf_id_t*)value;
        ebpf_object_dereference_by_id(id, EBPF_OBJECT_MAP);
    }

    _delete_hash_map(map);
}

static ebpf_result_t
_update_key_history(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key, bool remove)
{
    uint64_t now;
    ebpf_core_lru_map_t* lru_map;
    switch (map->ebpf_map_definition.type) {
    case BPF_MAP_TYPE_LRU_HASH:
    case BPF_MAP_TYPE_LRU_PERCPU_HASH:
        break;

    default:
        return EBPF_SUCCESS;
    }
    lru_map = EBPF_FROM_FIELD(ebpf_core_lru_map_t, core_map, map);
    now = ebpf_query_time_since_boot(true);

    if (!remove) {
        return ebpf_hash_table_update(lru_map->key_history, key, (uint8_t*)&now, EBPF_HASH_TABLE_OPERATION_ANY);
    } else {
        return ebpf_hash_table_delete(lru_map->key_history, key);
    }
}

static bool
_reap_oldest_map_entry(_In_ ebpf_core_map_t* map)
{
    uint8_t* previous_key = NULL;
    uint8_t* next_key = NULL;
    uint8_t* oldest_key = NULL;
    uint64_t* key_age = NULL;
    uint64_t oldest_key_age = MAXUINT64;
    ebpf_result_t result;
    ebpf_core_lru_map_t* lru_map;

    switch (map->ebpf_map_definition.type) {
    case BPF_MAP_TYPE_LRU_HASH:
    case BPF_MAP_TYPE_LRU_PERCPU_HASH:
        break;

    default:
        return false;
    }

    lru_map = EBPF_FROM_FIELD(ebpf_core_lru_map_t, core_map, map);

    // Walk through all the keys and values and find the oldest one.
    for (;;) {
        result = ebpf_hash_table_next_key_pointer_and_value(
            lru_map->key_history, previous_key, &next_key, (uint8_t**)&key_age);
        if (result != EBPF_SUCCESS) {
            break;
        }

        if (*key_age < oldest_key_age) {
            oldest_key_age = *key_age;
            oldest_key = next_key;
        }
        previous_key = next_key;
    }

    // If we reached the end of the keys, delete the oldest one found.
    if (result == EBPF_NO_MORE_KEYS && oldest_key != NULL) {
        ebpf_hash_table_delete((ebpf_hash_table_t*)lru_map->core_map.data, oldest_key);
        _update_key_history(map, oldest_key, true);
        return true;
    }
    return false;
}

static ebpf_result_t
_delete_hash_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key);

static ebpf_result_t
_find_hash_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data)
{
    uint8_t* value = NULL;
    if (!map || !key)
        return EBPF_INVALID_ARGUMENT;

    if (ebpf_hash_table_find((ebpf_hash_table_t*)map->data, key, &value) != EBPF_SUCCESS) {
        value = NULL;
    }

    if (value)
        _update_key_history(map, key, false);

    if (delete_on_success) {
        // Delete is atomic.
        // Only return value of both find and delete succeeded.
        if (_delete_hash_map_entry(map, key) != EBPF_SUCCESS) {
            value = NULL;
        }
    }

    *data = value;
    return value == NULL ? EBPF_OBJECT_NOT_FOUND : EBPF_SUCCESS;
}

/**
 * @brief Get an object from a map entry that holds objects, such
 * as a hash of maps.  The object returned holds a
 * reference that the caller is responsible for releasing.
 *
 * @param[in] map Hash map to search.
 * @param[in] key Pointer to the key to search for.
 * @returns Object pointer, or NULL if none.
 */
static _Ret_maybenull_ ebpf_object_t*
_get_object_from_hash_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    // We need to take a lock here to make sure we can
    // safely reference the object when another thread
    // might be trying to delete the entry we find.
    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);

    ebpf_object_t* object = NULL;
    uint8_t* value = NULL;
    if (_find_hash_map_entry(map, key, false, &value) == EBPF_SUCCESS) {
        ebpf_id_t id = *(ebpf_id_t*)value;
        (void)ebpf_object_reference_by_id(id, EBPF_OBJECT_MAP, &object);
    }

    ebpf_lock_unlock(&map->lock, lock_state);

    return object;
}

static ebpf_result_t
_update_hash_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_opt_ const uint8_t* data, ebpf_map_option_t option)
{
    ebpf_result_t result;
    size_t entry_count = 0;
    uint8_t* value;
    ebpf_hash_table_operations_t hash_table_operation;

    if (!map || !key)
        return EBPF_INVALID_ARGUMENT;

    switch (option) {
    case EBPF_ANY:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_ANY;
        break;
    case EBPF_NOEXIST:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_INSERT;
        break;
    case EBPF_EXIST:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_REPLACE;
        break;
    default:
        return EBPF_INVALID_ARGUMENT;
    }

    entry_count = ebpf_hash_table_key_count((ebpf_hash_table_t*)map->data);

    if ((entry_count >= map->ebpf_map_definition.max_entries) &&
        (ebpf_hash_table_find((ebpf_hash_table_t*)map->data, key, &value) != EBPF_SUCCESS) &&
        !_reap_oldest_map_entry(map))
        result = EBPF_OUT_OF_SPACE;
    else
        result = ebpf_hash_table_update((ebpf_hash_table_t*)map->data, key, data, hash_table_operation);

    if (result == EBPF_SUCCESS)
        _update_key_history(map, key, false);

    return result;
}

static ebpf_result_t
_update_hash_map_entry_with_handle(
    _In_ ebpf_core_map_t* map,
    _In_ const uint8_t* key,
    ebpf_object_type_t value_type,
    ebpf_handle_t value_handle,
    ebpf_map_option_t option)
{
    ebpf_result_t result = EBPF_SUCCESS;
    size_t entry_count = 0;
    if (!map || !key)
        return EBPF_INVALID_ARGUMENT;

    ebpf_hash_table_operations_t hash_table_operation;
    switch (option) {
    case EBPF_ANY:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_ANY;
        break;
    case EBPF_NOEXIST:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_INSERT;
        break;
    case EBPF_EXIST:
        hash_table_operation = EBPF_HASH_TABLE_OPERATION_REPLACE;
        break;
    default:
        return EBPF_INVALID_ARGUMENT;
    }

    ebpf_lock_state_t lock_state = ebpf_lock_lock(&map->lock);

    entry_count = ebpf_hash_table_key_count((ebpf_hash_table_t*)map->data);

    uint8_t* old_value = NULL;
    ebpf_result_t found_result = ebpf_hash_table_find((ebpf_hash_table_t*)map->data, key, &old_value);
    ebpf_id_t old_id = (old_value) ? *(ebpf_id_t*)old_value : 0;
    ebpf_object_t* value_object = NULL;

    if ((entry_count == map->ebpf_map_definition.max_entries) && (found_result != EBPF_SUCCESS)) {
        // The hash table is already full.
        result = EBPF_OUT_OF_SPACE;
        goto Done;
    }

    result = _get_map_value_object(map, value_handle, value_type, &value_object);
    if (result != EBPF_SUCCESS) {
        goto Done;
    }

    // Store the new object ID as the value.
    result =
        ebpf_hash_table_update((ebpf_hash_table_t*)map->data, key, (uint8_t*)&value_object->id, hash_table_operation);
    if (result != EBPF_SUCCESS) {
        goto Done;
    }

    // Release the reference on the old ID stored here, if any.
    if (old_id) {
        ebpf_object_dereference_by_id(old_id, value_type);
    }

Done:
    if ((result != EBPF_SUCCESS) && (value_object != NULL)) {
        ebpf_object_release_reference((ebpf_object_t*)value_object);
    }
    ebpf_lock_unlock(&map->lock, lock_state);
    return result;
}

static ebpf_result_t
_update_map_hash_map_entry_with_handle(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, uintptr_t value_handle, ebpf_map_option_t option)
{
    return _update_hash_map_entry_with_handle(map, key, EBPF_OBJECT_MAP, value_handle, option);
}

static ebpf_result_t
_delete_hash_map_entry_with_reference(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key, ebpf_object_type_t value_type)
{
    ebpf_result_t result;
    if (!map || !key)
        return EBPF_INVALID_ARGUMENT;

    if (value_type != EBPF_OBJECT_UNKNOWN) {
        uint8_t* value = NULL;
        if (ebpf_hash_table_find((ebpf_hash_table_t*)map->data, key, &value) == EBPF_SUCCESS) {
            ebpf_id_t id = *(ebpf_id_t*)value;
            ebpf_object_dereference_by_id(id, value_type);
        }
    }
    result = ebpf_hash_table_delete((ebpf_hash_table_t*)map->data, key);
    return result;
}

static ebpf_result_t
_delete_hash_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    _update_key_history(map, key, true);
    return _delete_hash_map_entry_with_reference(map, key, EBPF_OBJECT_UNKNOWN);
}

static ebpf_result_t
_delete_map_hash_map_entry(_In_ ebpf_core_map_t* map, _In_ const uint8_t* key)
{
    return _delete_hash_map_entry_with_reference(map, key, EBPF_OBJECT_MAP);
}

static ebpf_result_t
_next_hash_map_key(_In_ ebpf_core_map_t* map, _In_ const uint8_t* previous_key, _Out_ uint8_t* next_key)
{
    ebpf_result_t result;
    if (!map || !next_key)
        return EBPF_INVALID_ARGUMENT;

    result = ebpf_hash_table_next_key((ebpf_hash_table_t*)map->data, previous_key, next_key);
    return result;
}

static ebpf_result_t
_ebpf_adjust_value_pointer(_In_ ebpf_map_t* map, _Inout_ uint8_t** value)
{
    uint32_t current_cpu;
    uint32_t max_cpu = map->ebpf_map_definition.value_size / PAD_CACHE(map->original_value_size);
    switch (map->ebpf_map_definition.type) {
    case BPF_MAP_TYPE_PERCPU_ARRAY:
    case BPF_MAP_TYPE_PERCPU_HASH:
    case BPF_MAP_TYPE_LRU_PERCPU_HASH:
        break;
    default:
        return EBPF_SUCCESS;
    }
    current_cpu = ebpf_get_current_cpu();

    if (current_cpu > max_cpu) {
        return EBPF_INVALID_ARGUMENT;
    }
    (*value) += PAD_CACHE((size_t)map->original_value_size) * current_cpu;
    return EBPF_SUCCESS;
}

/**
 * @brief Insert the supplied value into the per-cpu value buffer of the map.
 * If the map doesn't contain an existing value, create a new all-zero value,
 * insert it, then set the per-cpu value. Note: This races with updates to the
 * value buffer from user mode.
 *
 * @param[in] map Map to update.
 * @param[in] key Key to search for.
 * @param[in] value Value to insert.
 * @retval EBPF_SUCCESS The operation was successful.
 * @retval EBPF_NO_MEMORY Unable to allocate resources for this
 *  entry.
 * @retval EBPF_INVALID_ARGUMENT Unable to perform this operation due to
 * current CPU > allocated value buffer size.
 */
ebpf_result_t
_update_entry_per_cpu(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ const uint8_t* value, ebpf_map_option_t option)
{
    uint8_t* target;
    if (ebpf_map_function_tables[map->ebpf_map_definition.type].find_entry(map, key, false, &target) != EBPF_SUCCESS) {
        ebpf_result_t return_value =
            ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry(map, key, NULL, option);
        if (return_value != EBPF_SUCCESS) {
            return return_value;
        }
        if (ebpf_map_function_tables[map->ebpf_map_definition.type].find_entry(map, key, false, &target) !=
            EBPF_SUCCESS) {
            return EBPF_NO_MEMORY;
        }
    }
    if (_ebpf_adjust_value_pointer(map, &target) != EBPF_SUCCESS) {
        return EBPF_INVALID_ARGUMENT;
    }

    memcpy(target, value, ebpf_map_get_effective_value_size(map));
    return EBPF_SUCCESS;
}

static void
_lpm_extract(_In_ const uint8_t* value, _Outptr_ const uint8_t** data, _Out_ size_t* length_in_bits)
{
    uint32_t prefix_length = *(uint32_t*)value;
    *data = value;
    *length_in_bits = sizeof(uint32_t) * 8 + prefix_length;
}

static ebpf_core_map_t*
_create_lpm_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    size_t max_prefix_length = (map_definition->key_size - sizeof(uint32_t)) * 8 + 1;
    ebpf_core_lpm_map_t* map = (ebpf_core_lpm_map_t*)_create_hash_map_internal(
        EBPF_OFFSET_OF(ebpf_core_lpm_map_t, data) + ebpf_bitmap_size(max_prefix_length), map_definition, _lpm_extract);
    if (!map) {
        return NULL;
    }
    map->max_prefix = (uint32_t)max_prefix_length;
    ebpf_bitmap_initialize((ebpf_bitmap_t*)map->data, max_prefix_length);
    return &(map->core_map);
}

static ebpf_result_t
_find_lpm_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data)
{
    uint32_t* prefix_length = (uint32_t*)key;
    uint32_t original_prefix_length = *prefix_length;
    uint8_t* value = NULL;
    ebpf_core_lpm_map_t* trie_map = EBPF_FROM_FIELD(ebpf_core_lpm_map_t, core_map, map);
    if (!map || !key || delete_on_success)
        return EBPF_INVALID_ARGUMENT;

    ebpf_bitmap_cursor_t cursor;
    ebpf_bitmap_start_reverse_search((ebpf_bitmap_t*)trie_map->data, &cursor);
    while (*prefix_length != MAXUINT32) {
        *prefix_length = (uint32_t)ebpf_bitmap_reverse_search_next_bit(&cursor);
        if (_find_hash_map_entry(map, key, false, &value) == EBPF_SUCCESS) {
            break;
        }
    }
    *prefix_length = original_prefix_length;

    if (!value) {
        return EBPF_KEY_NOT_FOUND;
    } else {
        *data = value;
        return EBPF_SUCCESS;
    }
}

static ebpf_result_t
_update_lpm_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_opt_ const uint8_t* data, ebpf_map_option_t option)
{
    ebpf_core_lpm_map_t* trie_map = EBPF_FROM_FIELD(ebpf_core_lpm_map_t, core_map, map);
    uint32_t prefix_length = *(uint32_t*)key;
    if (prefix_length > trie_map->max_prefix) {
        return EBPF_INVALID_ARGUMENT;
    }

    ebpf_result_t result = _update_hash_map_entry(map, key, data, option);
    if (result == EBPF_SUCCESS) {
        ebpf_bitmap_set_bit((ebpf_bitmap_t*)trie_map->data, prefix_length, true);
    }
    return result;
}

static ebpf_core_map_t*
_create_queue_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    size_t queue_map_size =
        EBPF_OFFSET_OF(ebpf_core_queue_map_t, slots) + map_definition->max_entries * sizeof(uint8_t*);
    return _create_array_map_with_map_struct_size(queue_map_size, map_definition);
}

static void
_delete_queue_map(_In_ ebpf_core_map_t* map)
{
    ebpf_core_queue_map_t* queue_map = EBPF_FROM_FIELD(ebpf_core_queue_map_t, core_map, map);

    // Free all the elements stored in the queue.
    for (size_t i = 0; i < queue_map->core_map.ebpf_map_definition.max_entries; i++) {
        ebpf_epoch_free(queue_map->slots[i]);
    }
    ebpf_epoch_free(queue_map);
}

static ebpf_result_t
_find_queue_map_entry(
    _In_ ebpf_core_map_t* map, _In_opt_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data)
{
    // Queue uses no key, so key must be NULL.
    if (!map || key)
        return EBPF_INVALID_ARGUMENT;

    ebpf_core_queue_map_t* queue_map = EBPF_FROM_FIELD(ebpf_core_queue_map_t, core_map, map);
    ebpf_lock_state_t state = ebpf_lock_lock(&queue_map->core_map.lock);
    if (queue_map->start == queue_map->end) {
        *data = NULL;
    } else {
        *data = queue_map->slots[queue_map->start];
        if (delete_on_success) {
            ebpf_epoch_free(queue_map->slots[queue_map->start]);
            queue_map->slots[queue_map->start] = NULL;
            queue_map->start = (queue_map->start + 1) % queue_map->core_map.ebpf_map_definition.max_entries;
        }
    }
    ebpf_lock_unlock(&queue_map->core_map.lock, state);
    return *data == NULL ? EBPF_OBJECT_NOT_FOUND : EBPF_SUCCESS;
}

static ebpf_result_t
_update_queue_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_opt_ const uint8_t* data, ebpf_map_option_t option)
{
    ebpf_result_t result;
    // Queue uses no key, so key must be NULL.
    if (!map || key || (option == EBPF_NOEXIST) || !data)
        return EBPF_INVALID_ARGUMENT;

    ebpf_core_queue_map_t* queue_map = EBPF_FROM_FIELD(ebpf_core_queue_map_t, core_map, map);
    ebpf_lock_state_t state = ebpf_lock_lock(&queue_map->core_map.lock);

    // Check for queue full.
    if (queue_map->start == ((queue_map->end + 1) % queue_map->core_map.ebpf_map_definition.max_entries)) {
        result = EBPF_OUT_OF_SPACE;
        goto Done;
    }

    uint8_t* value = ebpf_epoch_allocate(queue_map->core_map.ebpf_map_definition.value_size);
    if (!value) {
        result = EBPF_NO_MEMORY;
        goto Done;
    }
    memcpy(value, data, queue_map->core_map.ebpf_map_definition.value_size);
    queue_map->slots[queue_map->end] = value;

    queue_map->end = (queue_map->end + 1) % queue_map->core_map.ebpf_map_definition.max_entries;
    result = EBPF_SUCCESS;

Done:
    ebpf_lock_unlock(&queue_map->core_map.lock, state);
    return result;
}

static ebpf_core_map_t*
_create_stack_map(_In_ const ebpf_map_definition_in_memory_t* map_definition)
{
    size_t stack_map_size =
        EBPF_OFFSET_OF(ebpf_core_stack_map_t, slots) + map_definition->max_entries * sizeof(uint8_t*);
    return _create_array_map_with_map_struct_size(stack_map_size, map_definition);
}

static void
_delete_stack_map(_In_ ebpf_core_map_t* map)
{
    ebpf_core_stack_map_t* stack_map = EBPF_FROM_FIELD(ebpf_core_stack_map_t, core_map, map);
    // Free all the elements stored in the stack.
    for (size_t i = 0; i < stack_map->core_map.ebpf_map_definition.max_entries; i++) {
        ebpf_epoch_free(stack_map->slots[i]);
    }
    ebpf_epoch_free(stack_map);
}

static ebpf_result_t
_find_stack_map_entry(
    _In_ ebpf_core_map_t* map, _In_opt_ const uint8_t* key, _In_ bool delete_on_success, _Outptr_ uint8_t** data)
{
    // Stack uses no key, so key must be NULL.
    if (!map || key)
        return EBPF_INVALID_ARGUMENT;

    ebpf_core_stack_map_t* stack_map = EBPF_FROM_FIELD(ebpf_core_stack_map_t, core_map, map);
    ebpf_lock_state_t state = ebpf_lock_lock(&stack_map->core_map.lock);
    if (stack_map->top_of_stack == 0) {
        *data = NULL;
    } else {
        *data = stack_map->slots[stack_map->top_of_stack - 1];
        if (delete_on_success) {
            ebpf_epoch_free(stack_map->slots[stack_map->top_of_stack - 1]);
            stack_map->slots[stack_map->top_of_stack - 1] = NULL;
            stack_map->top_of_stack--;
        }
    }
    ebpf_lock_unlock(&stack_map->core_map.lock, state);
    return *data == NULL ? EBPF_OBJECT_NOT_FOUND : EBPF_SUCCESS;
}

static ebpf_result_t
_update_stack_map_entry(
    _In_ ebpf_core_map_t* map, _In_ const uint8_t* key, _In_opt_ const uint8_t* data, ebpf_map_option_t option)
{
    ebpf_result_t result;
    // Stack uses no key, so key must be NULL.
    if (!map || key || (option == EBPF_NOEXIST) || !data)
        return EBPF_INVALID_ARGUMENT;

    ebpf_core_stack_map_t* stack_map = EBPF_FROM_FIELD(ebpf_core_stack_map_t, core_map, map);
    ebpf_lock_state_t state = ebpf_lock_lock(&stack_map->core_map.lock);

    // Check for stack full.
    if (stack_map->top_of_stack >= stack_map->core_map.ebpf_map_definition.max_entries) {
        result = EBPF_OUT_OF_SPACE;
        goto Done;
    }

    uint8_t* value = ebpf_epoch_allocate(stack_map->core_map.ebpf_map_definition.value_size);
    if (!value) {
        result = EBPF_NO_MEMORY;
        goto Done;
    }
    memcpy(value, data, stack_map->core_map.ebpf_map_definition.value_size);
    stack_map->slots[stack_map->top_of_stack] = value;
    stack_map->top_of_stack++;
    result = EBPF_SUCCESS;

Done:
    ebpf_lock_unlock(&stack_map->core_map.lock, state);
    return result;
}

ebpf_map_function_table_t ebpf_map_function_tables[] = {
    {// BPF_MAP_TYPE_UNSPECIFIED
     NULL},
    {// BPF_MAP_TYPE_HASH
     _create_hash_map,
     _delete_hash_map,
     NULL,
     _find_hash_map_entry,
     NULL,
     _update_hash_map_entry,
     NULL,
     NULL,
     _delete_hash_map_entry,
     _next_hash_map_key},
    {// BPF_MAP_TYPE_ARRAY
     _create_array_map,
     _delete_array_map,
     NULL,
     _find_array_map_entry,
     NULL,
     _update_array_map_entry,
     NULL,
     NULL,
     _delete_array_map_entry,
     _next_array_map_key},
    {// BPF_MAP_TYPE_PROG_ARRAY
     _create_object_array_map,
     _delete_program_array_map,
     _associate_program_with_prog_array_map,
     _find_array_map_entry,
     _get_object_from_array_map_entry,
     NULL,
     _update_prog_array_map_entry_with_handle,
     NULL,
     _delete_program_array_map_entry,
     _next_array_map_key},
    {// BPF_MAP_TYPE_PERCPU_HASH
     _create_hash_map,
     _delete_hash_map,
     NULL,
     _find_hash_map_entry,
     NULL,
     _update_hash_map_entry,
     NULL,
     _update_entry_per_cpu,
     _delete_hash_map_entry,
     _next_hash_map_key},
    {// BPF_MAP_TYPE_PERCPU_ARRAY
     _create_array_map,
     _delete_array_map,
     NULL,
     _find_array_map_entry,
     NULL,
     _update_array_map_entry,
     NULL,
     _update_entry_per_cpu,
     _delete_array_map_entry,
     _next_array_map_key},
    {// BPF_MAP_TYPE_HASH_OF_MAPS
     _create_object_hash_map,
     _delete_object_hash_map,
     NULL,
     _find_hash_map_entry,
     _get_object_from_hash_map_entry,
     NULL,
     _update_map_hash_map_entry_with_handle,
     NULL,
     _delete_map_hash_map_entry,
     _next_array_map_key},
    {// BPF_MAP_TYPE_ARRAY_OF_MAPS
     _create_object_array_map,
     _delete_map_array_map,
     NULL,
     _find_array_map_entry,
     _get_object_from_array_map_entry,
     NULL,
     _update_map_array_map_entry_with_handle,
     NULL,
     _delete_map_array_map_entry,
     _next_array_map_key},
    {// BPF_MAP_TYPE_LRU_HASH
     _create_lru_hash_map,
     _delete_lru_hash_map,
     NULL,
     _find_hash_map_entry,
     NULL,
     _update_hash_map_entry,
     NULL,
     NULL,
     _delete_hash_map_entry,
     _next_hash_map_key},
    // LPM_TRIE is currently a hash-map with special behavior for find.
    {// BPF_MAP_TYPE_LPM_TRIE
     _create_lpm_map,
     _delete_hash_map,
     NULL,
     _find_lpm_map_entry,
     NULL,
     _update_lpm_map_entry,
     NULL,
     NULL,
     _delete_hash_map_entry,
     _next_hash_map_key},
    {// BPF_MAP_TYPE_QUEUE
     _create_queue_map,
     _delete_queue_map,
     NULL,
     _find_queue_map_entry,
     NULL,
     _update_queue_map_entry,
     NULL,
     NULL,
     NULL,
     NULL},
    {// BPF_MAP_TYPE_LRU_PERCPU_HASH
     _create_lru_hash_map,
     _delete_lru_hash_map,
     NULL,
     _find_hash_map_entry,
     NULL,
     _update_hash_map_entry,
     NULL,
     _update_entry_per_cpu,
     _delete_hash_map_entry,
     _next_hash_map_key},
    {// BPF_MAP_TYPE_STACK
     _create_stack_map,
     _delete_stack_map,
     NULL,
     _find_stack_map_entry,
     NULL,
     _update_stack_map_entry,
     NULL,
     NULL,
     NULL,
     NULL},

};

inline static void
_ebpf_map_signal_async_contexts(_In_ ebpf_core_map_t* map)
{
    // Skip if no async_contexts have ever been queued.
    if (!map->async_contexts_trip_wire) {
        return;
    }

    ebpf_lock_state_t state = ebpf_lock_lock(&map->lock);
    while (!ebpf_list_is_empty(&map->async_contexts)) {
        ebpf_core_map_async_context_t* context =
            EBPF_FROM_FIELD(ebpf_core_map_async_context_t, entry, map->async_contexts.Flink);
        ebpf_list_remove_entry(&context->entry);
        ebpf_async_complete(context->async_context, EBPF_SUCCESS);
        ebpf_free(context);
    }
    ebpf_lock_unlock(&map->lock, state);
}

static void
_ebpf_map_delete(_In_ ebpf_object_t* object)
{
    EBPF_LOG_ENTRY();
    ebpf_map_t* map = (ebpf_map_t*)object;

    if (map->inner_map_template != NULL) {
        ebpf_object_release_reference(&map->inner_map_template->object);
    }
    ebpf_free(map->name.value);
    _ebpf_map_signal_async_contexts(map);
    ebpf_map_function_tables[map->ebpf_map_definition.type].delete_map(map);
    EBPF_RETURN_VOID();
}

ebpf_result_t
ebpf_map_create(
    _In_ const ebpf_utf8_string_t* map_name,
    _In_ const ebpf_map_definition_in_memory_t* ebpf_map_definition,
    ebpf_handle_t inner_map_handle,
    _Outptr_ ebpf_map_t** ebpf_map)
{
    EBPF_LOG_ENTRY();
    ebpf_map_t* local_map = NULL;
    ebpf_object_t* inner_map_template_object = NULL;
    ebpf_map_type_t type = ebpf_map_definition->type;
    ebpf_result_t result = EBPF_SUCCESS;
    uint32_t cpu_count;
    cpu_count = ebpf_get_cpu_count();
    ebpf_map_definition_in_memory_t local_map_definition = *ebpf_map_definition;
    switch (local_map_definition.type) {
    case BPF_MAP_TYPE_PERCPU_HASH:
    case BPF_MAP_TYPE_PERCPU_ARRAY:
        local_map_definition.value_size = cpu_count * PAD_CACHE(local_map_definition.value_size);
        break;
    default:
        break;
    }

    if (type >= EBPF_COUNT_OF(ebpf_map_function_tables)) {
        EBPF_LOG_MESSAGE_UINT64(EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Unsupported map type", type);
        result = EBPF_INVALID_ARGUMENT;
        goto Exit;
    }

    if (map_name->length >= BPF_OBJ_NAME_LEN) {
        EBPF_LOG_MESSAGE_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Map name too long", map_name->length);
        result = EBPF_INVALID_ARGUMENT;
        goto Exit;
    }

    if (!ebpf_map_function_tables[type].create_map) {
        EBPF_LOG_MESSAGE_UINT64(EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Unsupported map type", type);
        result = EBPF_OPERATION_NOT_SUPPORTED;
        goto Exit;
    }

    if (local_map_definition.size != sizeof(local_map_definition)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Unsupported map definition size",
            local_map_definition.size,
            sizeof(local_map_definition));
        result = EBPF_INVALID_ARGUMENT;
        goto Exit;
    }

    if (local_map_definition.type == BPF_MAP_TYPE_ARRAY_OF_MAPS ||
        local_map_definition.type == BPF_MAP_TYPE_HASH_OF_MAPS) {
        if (inner_map_handle == ebpf_handle_invalid) {
            // Must have a valid inner_map_handle.
            EBPF_LOG_MESSAGE(
                EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Map in map must have a valid inner_map_handle");
            result = EBPF_INVALID_FD;
            goto Exit;
        }

        // Convert value handle to an object pointer.
        result = ebpf_reference_object_by_handle(inner_map_handle, EBPF_OBJECT_MAP, &inner_map_template_object);
        if (result != EBPF_SUCCESS)
            goto Exit;
    }

    local_map = ebpf_map_function_tables[type].create_map(&local_map_definition);
    if (!local_map) {
        result = EBPF_NO_MEMORY;
        goto Exit;
    }

    local_map->original_value_size = ebpf_map_definition->value_size;

    result = ebpf_duplicate_utf8_string(&local_map->name, map_name);
    if (result != EBPF_SUCCESS) {
        goto Exit;
    }

    ebpf_map_function_table_t* table = &ebpf_map_function_tables[local_map->ebpf_map_definition.type];
    ebpf_object_get_program_type_t get_program_type = (table->get_object_from_entry) ? _get_map_program_type : NULL;
    result = ebpf_object_initialize(&local_map->object, EBPF_OBJECT_MAP, _ebpf_map_delete, get_program_type);
    if (result != EBPF_SUCCESS) {
        goto Exit;
    }

    local_map->inner_map_template = (ebpf_map_t*)inner_map_template_object;
    *ebpf_map = local_map;

    ebpf_list_initialize(&local_map->async_contexts);

Exit:
    if (result != EBPF_SUCCESS) {
        if (local_map) {
            ebpf_object_release_reference(inner_map_template_object);
            ebpf_free(local_map->name.value);
            ebpf_epoch_free(local_map);
        }
    }
    EBPF_RETURN_RESULT(result);
}

ebpf_result_t
ebpf_map_find_entry(
    _In_ ebpf_map_t* map,
    size_t key_size,
    _In_reads_(key_size) const uint8_t* key,
    size_t value_size,
    _Out_writes_(value_size) uint8_t* value,
    int flags)
{
    // High volume call - Skip entry/exit logging.
    uint8_t* return_value = NULL;
    if (!(flags & EBPF_MAP_FLAG_HELPER) && (key_size != map->ebpf_map_definition.key_size)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return EBPF_INVALID_ARGUMENT;
    }

    if (!(flags & EBPF_MAP_FLAG_HELPER) && (value_size != map->ebpf_map_definition.value_size)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map value size",
            value_size,
            map->ebpf_map_definition.value_size);
        return EBPF_INVALID_ARGUMENT;
    }

    ebpf_map_type_t type = map->ebpf_map_definition.type;
    if ((flags & EBPF_MAP_FLAG_HELPER) && (ebpf_map_function_tables[type].get_object_from_entry != NULL)) {

        // Disallow reads to prog array maps from this helper call for now.
        if (type == BPF_MAP_TYPE_PROG_ARRAY) {
            EBPF_LOG_MESSAGE(
                EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Find not supported on BPF_MAP_TYPE_PROG_ARRAY");
            return EBPF_INVALID_ARGUMENT;
        }

        ebpf_object_t* object = ebpf_map_function_tables[type].get_object_from_entry(map, key);

        // Release the extra reference obtained.
        // REVIEW: is this safe?
        if (object) {
            ebpf_object_release_reference(object);
            return_value = (uint8_t*)object;
        }
    } else {
        ebpf_result_t result = ebpf_map_function_tables[map->ebpf_map_definition.type].find_entry(
            map, key, flags & EPBF_MAP_FIND_FLAG_DELETE ? true : false, &return_value);
        if (result != EBPF_SUCCESS) {
            return result;
        }
    }
    if (return_value == NULL) {
        return EBPF_OBJECT_NOT_FOUND;
    }

    if (flags & EBPF_MAP_FLAG_HELPER) {
        if (_ebpf_adjust_value_pointer(map, &return_value) != EBPF_SUCCESS) {
            return EBPF_INVALID_ARGUMENT;
        }

        *(uint8_t**)value = return_value;
    } else {
        memcpy(value, return_value, map->ebpf_map_definition.value_size);
    }
    if (flags & EPBF_MAP_FIND_FLAG_DELETE) {
        _ebpf_map_signal_async_contexts(map);
    }
    return EBPF_SUCCESS;
}

ebpf_result_t
ebpf_map_associate_program(_In_ ebpf_map_t* map, _In_ const ebpf_program_t* program)
{
    EBPF_LOG_ENTRY();
    if (ebpf_map_function_tables[map->ebpf_map_definition.type].associate_program)
        return ebpf_map_function_tables[map->ebpf_map_definition.type].associate_program(map, program);
    EBPF_RETURN_RESULT(EBPF_SUCCESS);
}

_Ret_maybenull_ ebpf_program_t*
ebpf_map_get_program_from_entry(_In_ ebpf_map_t* map, size_t key_size, _In_reads_(key_size) const uint8_t* key)
{
    // High volume call - Skip entry/exit logging.
    if (key_size != map->ebpf_map_definition.key_size) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return NULL;
    }
    ebpf_map_type_t type = map->ebpf_map_definition.type;
    if (type != BPF_MAP_TYPE_PROG_ARRAY) {
        EBPF_LOG_MESSAGE_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR, EBPF_TRACELOG_KEYWORD_MAP, "Operation not supported on map", type);
        return NULL;
    }

    if (ebpf_map_function_tables[type].get_object_from_entry == NULL) {
        return NULL;
    }
    return (ebpf_program_t*)ebpf_map_function_tables[type].get_object_from_entry(map, key);
}

ebpf_result_t
ebpf_map_update_entry(
    _In_ ebpf_map_t* map,
    size_t key_size,
    _In_reads_(key_size) const uint8_t* key,
    size_t value_size,
    _In_reads_(value_size) const uint8_t* value,
    ebpf_map_option_t option,
    int flags)
{
    // High volume call - Skip entry/exit logging.
    ebpf_result_t result;
    if (!(flags & EBPF_MAP_FLAG_HELPER) && (key_size != map->ebpf_map_definition.key_size)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return EBPF_INVALID_ARGUMENT;
    }

    if (!(flags & EBPF_MAP_FLAG_HELPER) && (value_size != map->ebpf_map_definition.value_size)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map value size",
            value_size,
            map->ebpf_map_definition.value_size);
        return EBPF_INVALID_ARGUMENT;
    }

    if ((ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry == NULL) ||
        (ebpf_map_function_tables[map->ebpf_map_definition.type].find_entry == NULL)) {
        EBPF_LOG_MESSAGE_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "ebpf_map_update_entry not supported on map",
            map->ebpf_map_definition.type);
        return EBPF_INVALID_ARGUMENT;
    }

    if ((flags & EBPF_MAP_FLAG_HELPER) &&
        ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry_per_cpu) {
        result = ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry_per_cpu(map, key, value, option);
    } else {
        result = ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry(map, key, value, option);
    }
    if (result == EBPF_SUCCESS) {
        _ebpf_map_signal_async_contexts(map);
    }
    return result;
}

ebpf_result_t
ebpf_map_update_entry_with_handle(
    _In_ ebpf_map_t* map,
    size_t key_size,
    _In_reads_(key_size) const uint8_t* key,
    uintptr_t value_handle,
    ebpf_map_option_t option)
{
    // High volume call - Skip entry/exit logging.
    if (key_size != map->ebpf_map_definition.key_size) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return EBPF_INVALID_ARGUMENT;
    }

    if (ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry_with_handle == NULL) {
        EBPF_LOG_MESSAGE_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "ebpf_map_update_entry_with_handle not supported on map",
            map->ebpf_map_definition.type);
        return EBPF_OPERATION_NOT_SUPPORTED;
    }
    return ebpf_map_function_tables[map->ebpf_map_definition.type].update_entry_with_handle(
        map, key, value_handle, option);
}

ebpf_result_t
ebpf_map_delete_entry(_In_ ebpf_map_t* map, size_t key_size, _In_reads_(key_size) const uint8_t* key, int flags)
{
    // High volume call - Skip entry/exit logging.
    if (!(flags & EBPF_MAP_FLAG_HELPER) && (key_size != map->ebpf_map_definition.key_size)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return EBPF_INVALID_ARGUMENT;
    }
    ebpf_result_t result = ebpf_map_function_tables[map->ebpf_map_definition.type].delete_entry(map, key);
    if (result == EBPF_SUCCESS) {
        _ebpf_map_signal_async_contexts(map);
    }
    return result;
}

ebpf_result_t
ebpf_map_next_key(
    _In_ ebpf_map_t* map,
    size_t key_size,
    _In_reads_opt_(key_size) const uint8_t* previous_key,
    _Out_writes_(key_size) uint8_t* next_key)
{
    // High volume call - Skip entry/exit logging.
    if (key_size != map->ebpf_map_definition.key_size) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "Incorrect map key size",
            key_size,
            map->ebpf_map_definition.key_size);
        return EBPF_INVALID_ARGUMENT;
    }
    return ebpf_map_function_tables[map->ebpf_map_definition.type].next_key(map, previous_key, next_key);
}

ebpf_result_t
ebpf_map_get_info(
    _In_ const ebpf_map_t* map, _Out_writes_to_(*info_size, *info_size) uint8_t* buffer, _Inout_ uint16_t* info_size)
{
    // High volume call - Skip entry/exit logging.
    struct bpf_map_info* info = (struct bpf_map_info*)buffer;

    if (*info_size < sizeof(*info)) {
        EBPF_LOG_MESSAGE_UINT64_UINT64(
            EBPF_TRACELOG_LEVEL_ERROR,
            EBPF_TRACELOG_KEYWORD_MAP,
            "ebpf_map_get_info buffer too small",
            *info_size,
            sizeof(*info));
        return EBPF_INSUFFICIENT_BUFFER;
    }

    info->id = map->object.id;
    info->type = map->ebpf_map_definition.type;
    info->key_size = map->ebpf_map_definition.key_size;
    info->value_size = map->original_value_size;
    info->max_entries = map->ebpf_map_definition.max_entries;
    info->inner_map_id = (map->inner_map_template) ? map->inner_map_template->object.id : EBPF_ID_NONE;
    info->pinned_path_count = map->object.pinned_path_count;
    strncpy_s(info->name, sizeof(info->name), (char*)map->name.value, map->name.length);

    *info_size = sizeof(*info);
    return EBPF_SUCCESS;
}

static void
_ebpf_map_cancel_context(_In_ _Frees_ptr_ void* cancel_context)
{
    ebpf_core_map_async_context_t* context = (ebpf_core_map_async_context_t*)cancel_context;
    ebpf_lock_state_t state = ebpf_lock_lock(&context->map->lock);
    ebpf_list_remove_entry(&context->entry);
    ebpf_lock_unlock(&context->map->lock, state);
    ebpf_async_complete(context->async_context, EBPF_SUCCESS);
    ebpf_free(context);
}

ebpf_result_t
ebpf_map_wait_for_update(_Inout_ ebpf_map_t* map, _In_ void* async_context)
{
    ebpf_core_map_async_context_t* context = ebpf_allocate(sizeof(ebpf_core_map_async_context_t));
    if (!context) {
        return EBPF_NO_MEMORY;
    }
    ebpf_list_initialize(&context->entry);
    context->async_context = async_context;
    context->map = map;

    ebpf_lock_state_t state = ebpf_lock_lock(&map->lock);
    ebpf_list_insert_tail(&map->async_contexts, &context->entry);
    ebpf_lock_unlock(&map->lock, state);

    ebpf_async_set_cancel_callback(async_context, context, _ebpf_map_cancel_context);
    map->async_contexts_trip_wire = true;
    return EBPF_PENDING;
}

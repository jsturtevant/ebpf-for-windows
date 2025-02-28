// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

#pragma once

#include "ebpf_api.h"
#include "platform.h"

// Device type
#define EBPF_IOCTL_TYPE FILE_DEVICE_NETWORK

// Function codes from 0x800 to 0xFFF are for customer use.
#define IOCTL_EBPFCTL_METHOD_BUFFERED CTL_CODE(EBPF_IOCTL_TYPE, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Maxiumum attempts to invoke an IOCTL.
#define IOCTL_MAX_ATTEMPTS 16

typedef std::vector<uint8_t> ebpf_protocol_buffer_t;
typedef std::vector<uint8_t> ebpf_code_buffer_t;

typedef struct empty_reply
{
} empty_reply_t;

static empty_reply_t _empty_reply;

ebpf_result_t
initialize_device_handle();

void
clean_up_device_handle();

ebpf_handle_t
get_device_handle();

template <typename request_t, typename reply_t = empty_reply_t>
uint32_t
invoke_ioctl(request_t& request, reply_t& reply = _empty_reply, OVERLAPPED* overlapped = nullptr)
{
    uint32_t return_value = ERROR_SUCCESS;
    uint32_t actual_reply_size;
    uint32_t request_size;
    void* request_ptr;
    uint32_t reply_size;
    void* reply_ptr;
    bool variable_reply_size = false;

    if constexpr (std::is_same<request_t, nullptr_t>::value) {
        request_size = 0;
        request_ptr = nullptr;
    } else if constexpr (std::is_same<request_t, ebpf_protocol_buffer_t>::value) {
        request_size = static_cast<uint32_t>(request.size());
        request_ptr = request.data();
    } else {
        request_size = sizeof(request);
        request_ptr = &request;
    }

    if constexpr (std::is_same<reply_t, nullptr_t>::value) {
        reply_size = 0;
        reply_ptr = nullptr;
    } else if constexpr (std::is_same<reply_t, ebpf_protocol_buffer_t>::value) {
        reply_size = static_cast<uint32_t>(reply.size());
        reply_ptr = reply.data();
        variable_reply_size = true;
    } else if constexpr (std::is_same<reply_t, empty_reply>::value) {
        reply_size = 0;
        reply_ptr = nullptr;
    } else {
        reply_size = static_cast<uint32_t>(sizeof(reply));
        reply_ptr = &reply;
    }

    auto result = Platform::DeviceIoControl(
        get_device_handle(),
        IOCTL_EBPFCTL_METHOD_BUFFERED,
        request_ptr,
        request_size,
        reply_ptr,
        reply_size,
        &actual_reply_size,
        overlapped);

    if (!result) {
        return_value = GetLastError();
        goto Exit;
    }

    // Actual reply size cannot be smaller than minimum expected reply size.
    if (actual_reply_size < reply_size) {
        return_value = ERROR_INVALID_PARAMETER;
        goto Exit;
    }

    if (actual_reply_size != reply_size && !variable_reply_size) {
        return_value = ERROR_INVALID_PARAMETER;
        goto Exit;
    }

Exit:
    return return_value;
}
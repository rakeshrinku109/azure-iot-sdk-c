// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifdef __cplusplus
#include <cstdlib>
#include <cstddef>
#include <cstring>
#else
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#endif

#include "iothub.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"

#include "azure_c_shared_utility/envvariable.h"
#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/lock.h"

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
#include "certs.h"
#endif // SET_TRUSTED_CERT_IN_SAMPLES

#include "min_e2e_test_common.h"

static bool g_callbackRecv = false;
static TEST_PROTOCOL_TYPE test_protocol_type;

static const char* TEST_HOSTNAME_VALUE = "HostName=";
#define TEST_HOSTNAME_VALUE_LEN         9
static const char* TEST_DEVICEID_VALUE = "DeviceId=";
#define TEST_DEVICEID_VALUE_LEN         9
static const char* TEST_SHARED_ACCESS_KEY_VALUE = "SharedAccessKey=";
#define TEST_SHARED_ACCESS_KEY_LEN      16
static const char* TEST_BACK_COMPAT_MSG_STRING = "back_compat_message_string";
static const unsigned char TEST_BACK_COMPAT_MSG_BYTE[] = { 0x3, 0xb, 0xc };
static size_t TEST_MSG_BYTE_SIZE = 3;

static size_t g_iotHubTestId = 0;

#define MAX_OPERATION_TIMEOUT   2*60
#define DO_WORK_LOOP_COUNTER    20

typedef struct MIN_E2E_MSG_CTX_TAG
{
    bool callback_recv;
    bool connected;
    bool is_error;
    bool twin_callback_recv;
} MIN_E2E_MSG_CTX;

typedef struct CMD_CHANNEL_CTX_TAG
{
    bool twin_callback_recv;
} CMD_CHANNEL_CTX;

typedef struct MIN_E2E_TEST_INFO_TAG
{
    TEST_PROTOCOL_TYPE protocol;
    TICK_COUNTER_HANDLE tick_cntr_handle;
    TRANSPORT_HANDLE transport_handle;
    IOTHUB_DEVICE_CLIENT_LL_HANDLE device_handle;
    char msg_id[32];
    size_t d2c_msg_count;
    const char* connection_string;
    const char* x509_cert;
    const char* x509_key;
} MIN_E2E_TEST_INFO;

static void cleanup_config_object(IOTHUB_CLIENT_CONFIG* client_config)
{
    free((char*)client_config->deviceId);
    free((char*)client_config->iotHubName);
    free((char*)client_config->iotHubSuffix);
    free((char*)client_config->deviceKey);
}

static bool is_operation_timed_out(TICK_COUNTER_HANDLE tick_cntr_handle, tickcounter_ms_t compare_time, size_t timeout_seconds)
{
    bool result;
    tickcounter_ms_t curr_tick;
    if (tickcounter_get_current_ms(tick_cntr_handle, &curr_tick) != 0)
    {
        LogError("Failure getting tickcounter value");
        result = true;
    }
    else
    {
        result = (((curr_tick - compare_time) / 1000 > timeout_seconds));
    }
    return result;
}

static void openCompleteCallback(void* user_ctx)
{
    LogInfo("Open completed, context: %s", (char*)user_ctx);
}

static void message_recv_callback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* user_ctx)
{
    LogInfo("message_recv_callback invoked, result=<%s>, user_ctx=<%p>", MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result), user_ctx);

    MIN_E2E_MSG_CTX* back_compat_info = (MIN_E2E_MSG_CTX*)user_ctx;
    if (back_compat_info != NULL)
    {
        back_compat_info->callback_recv = true;
    }
}

static void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS status, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_ctx)
{
    (void)reason;
    LogInfo("connection_status_callback invoked, status=<%s>, user_ctx=<%p>", MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS, status), user_ctx);
    MIN_E2E_MSG_CTX* min_e2e_info = (MIN_E2E_MSG_CTX*)user_ctx;
    if (min_e2e_info != NULL)
    {
        if (status == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
        {
            min_e2e_info->connected = true;
        }
        else
        {
            min_e2e_info->connected = false;
            min_e2e_info->is_error = true;
            LogError("Authentication failed for sdk");
        }
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT c2d_msg_callback(IOTHUB_MESSAGE_HANDLE event_msg, void* user_ctx)
{
    LogInfo("c2d_msg_callback invoked, message=<%p>, user_ctx=<%p>", event_msg, user_ctx);

    // Test the properties from the message
    const char* test_value;
    test_value = IoTHubMessage_GetMessageId(event_msg);
    test_value = IoTHubMessage_GetCorrelationId(event_msg);
    test_value = IoTHubMessage_GetContentTypeSystemProperty(event_msg);
    test_value = IoTHubMessage_GetContentEncodingSystemProperty(event_msg);
    test_value = IoTHubMessage_GetOutputName(event_msg);
    test_value = IoTHubMessage_GetInputName(event_msg);
    test_value = IoTHubMessage_GetProperty(event_msg, "property_key");

    return IOTHUBMESSAGE_ACCEPTED;
}

static int method_callback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size, void* user_ctx)
{
    (void)payload;
    (void)size;
    (void)method_name;
    (void)user_ctx;
    (void)response;
    (void)response_size;
    /*BACK_COMPAT_MSG_CTX* back_compat_info = (BACK_COMPAT_MSG_CTX*)user_ctx;
    if (strcmp(method_name, "back_compat_method_name") == 0)
    {
        *response_size = 1;
        *response = (unsigned char*)malloc(*response_size);
        (void)memcpy(*response, "", *response_size);
    }*/
    return 200;
}

static void device_twin_callback(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payload, size_t size, void* user_ctx)
{
    (void)payload;
    (void)size;
    LogInfo("device_twin_callback invoked, status=<%s>, user_ctx=<%p>", MU_ENUM_TO_STRING(DEVICE_TWIN_UPDATE_STATE, update_state), user_ctx);

    /*BACK_COMPAT_MSG_CTX* back_compat_info = (BACK_COMPAT_MSG_CTX*)user_ctx;
    if (back_compat_info)
    {
        back_compat_info->twin_callback_recv = true;
    }*/
}

static bool check_iothub_result(IOTHUB_CLIENT_RESULT iothub_result)
{
    bool result;
    switch (iothub_result)
    {
        case IOTHUB_CLIENT_OK:
            result = true;
            break;
        case IOTHUB_CLIENT_INVALID_ARG:
        case IOTHUB_CLIENT_ERROR:
        case IOTHUB_CLIENT_INVALID_SIZE:
        case IOTHUB_CLIENT_INDEFINITE_TIME:
        default:
            result = false;
            break;
    }
    return result;
}

static int parse_connection_string(const char* connection_string, IOTHUB_CLIENT_CONFIG* client_config)
{
    int result = 0;
    //size_t tag_index = 0;
    size_t field_cnt = 0;
    size_t counter = 0;
    const char* initial_pos = NULL;
    const char* iterator = connection_string;
    while (iterator != NULL && *iterator != '\0' && result == 0 && field_cnt < 3)
    {
        if (memcmp(iterator, TEST_HOSTNAME_VALUE, TEST_HOSTNAME_VALUE_LEN) == 0)
        {
            counter = 0;
            iterator += TEST_HOSTNAME_VALUE_LEN;
            initial_pos = iterator;
            // Loop here and break up the hostname into it's parts
            while (*iterator != '\0' && result == 0)
            {
                if (*iterator == '.')
                {
                    if (client_config->iotHubName == NULL)
                    {
                        if ((client_config->iotHubName = (const char*)malloc(counter + 1)) == NULL)
                        {
                            LogError("Failure allocating iothub Name");
                            result = MU_FAILURE;
                            cleanup_config_object(client_config);
                        }
                        else
                        {
                            memset((char*)client_config->iotHubName, 0, counter + 1);
                            memcpy((char*)client_config->iotHubName, initial_pos, iterator - initial_pos);
                            // Change the initial position to the current pos beyond the .
                            initial_pos = iterator+1;
                            counter = 0;
                        }
                    }
                }
                else if (*iterator == ';')
                {
                    // Now get the suffix
                    if ((client_config->iotHubSuffix = (const char*)malloc(counter+1)) == NULL)
                    {
                        LogError("Failure allocating iothub suffix");
                        result = MU_FAILURE;
                        cleanup_config_object(client_config);
                    }
                    else
                    {
                        memset((char*)client_config->iotHubSuffix, 0, counter + 1);
                        memcpy((char*)client_config->iotHubSuffix, initial_pos, iterator - initial_pos);
                        initial_pos = iterator + 1;
                        counter = 0;
                    }
                    break;
                }
                iterator++;
                counter++;
            }
            field_cnt++;
        }
        else if (memcmp(iterator, TEST_DEVICEID_VALUE, TEST_DEVICEID_VALUE_LEN) == 0)
        {
            iterator += TEST_DEVICEID_VALUE_LEN;
            if (client_config->deviceId == NULL)
            {
                counter = 0;
                initial_pos = iterator;
                while (*iterator != '\0' && *iterator != ';')
                {
                    counter++;
                    iterator++;
                }

                if ((client_config->deviceId = (const char*)malloc(counter + 1)) == NULL)
                {
                    LogError("Failure allocating iothub device id");
                    result = MU_FAILURE;
                    cleanup_config_object(client_config);
                }
                else
                {
                    memset((char*)client_config->deviceId, 0, counter + 1);
                    memcpy((char*)client_config->deviceId, initial_pos, counter);
                }
            }
            else
            {
                LogError("invalid value state.  device previously allocated");
                result = MU_FAILURE;
            }
            field_cnt++;
        }
        else if (memcmp(iterator, TEST_SHARED_ACCESS_KEY_VALUE, TEST_SHARED_ACCESS_KEY_LEN) == 0)
        {
            iterator += TEST_SHARED_ACCESS_KEY_LEN;
            if (client_config->deviceKey == NULL)
            {
                counter = 0;
                initial_pos = iterator;
                while (*iterator != '\0' && *iterator != ';')
                {
                    counter++;
                    iterator++;
                }

                if ((client_config->deviceKey = (const char*)malloc(counter + 1)) == NULL)
                {
                    LogError("Failure allocating iothub device key");
                    result = MU_FAILURE;
                    cleanup_config_object(client_config);
                }
                else
                {
                    memset((char*)client_config->deviceKey, 0, counter + 1);
                    memcpy((char*)client_config->deviceKey, initial_pos, counter);
                }
            }
            field_cnt++;
        }
        iterator++;
    }
    return result;
}

static IOTHUB_DEVICE_CLIENT_LL_HANDLE create_device_client(MIN_E2E_TEST_INFO* min_e2e_info, DEVICE_CREATION_TYPE type, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, MIN_E2E_MSG_CTX* ctx)
{
    IOTHUB_DEVICE_CLIENT_LL_HANDLE result;

    switch (type)
    {
        case DEVICE_CREATION_CONN_STRING:
        {
            result = IoTHubDeviceClient_LL_CreateFromConnectionString(min_e2e_info->connection_string, protocol);
            break;
        }
        case DEVICE_CREATION_CREATE:
        {
            IOTHUB_CLIENT_CONFIG client_config = { 0 };
            if (parse_connection_string(min_e2e_info->connection_string, &client_config) != 0)
            {
                LogError("Failure parsing connection string");
                result = NULL;
            }
            else
            {
                client_config.protocol = protocol;
                result = IoTHubDeviceClient_LL_Create(&client_config);
                cleanup_config_object(&client_config);
            }
            break;
        }
        case DEVICE_CREATION_WITH_TRANSPORT:
        {
            IOTHUB_CLIENT_CONFIG client_config = { 0 };
            if (parse_connection_string(min_e2e_info->connection_string, &client_config) != 0)
            {
                LogError("Failure parsing connection string");
                result = NULL;
            }
            else
            {
                if ((min_e2e_info->transport_handle = IoTHubTransport_Create(protocol, client_config.iotHubName, client_config.iotHubSuffix)) == NULL)
                {
                    IOTHUB_CLIENT_DEVICE_CONFIG device_config;
                    device_config.deviceId = client_config.deviceId;
                    device_config.deviceKey = client_config.deviceKey;
                    device_config.protocol = protocol;
                    device_config.transportHandle = min_e2e_info->transport_handle;

                    result = IoTHubDeviceClient_LL_CreateWithTransport(&device_config);
                }
                else
                {
                    LogError("Failure creating transport");
                    result = NULL;
                }
                cleanup_config_object(&client_config);
            }
        }
        break;
        default:
        {
            LogError("Failure unknown creation type %d", (int)type);
            result = NULL;
            break;
        }
    }
    // Always set the log trace option
    if (result != NULL)
    {
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetConnectionStatusCallback(result, connection_status_callback, ctx)))
        {
            LogError("Failure unknown creation type %d", (int)type);
        }
        else
        {
            bool trace_on = true;
            IoTHubDeviceClient_LL_SetOption(result, OPTION_LOG_TRACE, &trace_on);
#ifdef SET_TRUSTED_CERT_IN_SAMPLES
            // Setting the Trusted Certificate.  This is only necessary on system with without
            // built in certificate stores.
            IoTHubDeviceClient_LL_SetOption(result, OPTION_TRUSTED_CERT, certificates);
#endif // SET_TRUSTED_CERT_IN_SAMPLES

        }
    }
    return result;
}

static int set_device_options(TEST_PROTOCOL_TYPE protocol_type, IOTHUB_DEVICE_CLIENT_LL_HANDLE device_client)
{
    int result = 0;

    size_t refresh_sas_token = 30 * 60;
    size_t sas_token_refresh = 30 * 60;
    tickcounter_ms_t msg_timeout = 2*60;

    if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_SAS_TOKEN_LIFETIME, &refresh_sas_token)))
    {
        LogError("Failure setting option OPTION_SAS_TOKEN_LIFETIME");
        result++;
    }
    if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_SAS_TOKEN_REFRESH_TIME, &sas_token_refresh)))
    {
        LogError("Failure setting option OPTION_SAS_TOKEN_REFRESH_TIME");
        result++;
    }
    if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_MESSAGE_TIMEOUT, &msg_timeout)))
    {
        LogError("Failure setting option OPTION_MESSAGE_TIMEOUT");
        result++;
    }
    if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_PRODUCT_INFO, "custom_product_info")))
    {
        LogError("Failure setting option OPTION_PRODUCT_INFO");
        result++;
    }
    // Proxy host???

    // MQTT only
    if (protocol_type == TEST_MQTT || protocol_type == TEST_MQTT_WEBSOCKETS)
    {
        int keep_alive = 5 * 60;
        int connection_timeout = 31;
        bool url_encode = true;
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_AUTO_URL_ENCODE_DECODE, &url_encode)))
        {
            LogError("Failure setting option OPTION_AUTO_URL_ENCODE_DECODE");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_KEEP_ALIVE, &keep_alive)))
        {
            LogError("Failure setting option OPTION_KEEP_ALIVE");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_CONNECTION_TIMEOUT, &connection_timeout)))
        {
            LogError("Failure setting option OPTION_CONNECTION_TIMEOUT");
            result++;
        }
    }
    else if (protocol_type == TEST_AMQP || protocol_type == TEST_AMQP_WEBSOCKETS)
    {
        size_t cbs_refresh = 35;
        size_t server_side_keep_alive = 241;
        size_t c2d_side_keep_alive = 241;
        double remote_idle_timeout = .08;
        size_t send_timeout = 301;

        // Amqp only
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_CBS_REQUEST_TIMEOUT, &cbs_refresh)))
        {
            LogError("Failure setting option OPTION_CBS_REQUEST_TIMEOUT");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_SERVICE_SIDE_KEEP_ALIVE_FREQ_SECS, &server_side_keep_alive)))
        {
            LogError("Failure setting option OPTION_SERVICE_SIDE_KEEP_ALIVE_FREQ_SECS");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_C2D_KEEP_ALIVE_FREQ_SECS, &c2d_side_keep_alive)))
        {
            LogError("Failure setting option OPTION_C2D_KEEP_ALIVE_FREQ_SECS");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_REMOTE_IDLE_TIMEOUT_RATIO, &remote_idle_timeout)))
        {
            LogError("Failure setting option OPTION_REMOTE_IDLE_TIMEOUT_RATIO");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_EVENT_SEND_TIMEOUT_SECS, &send_timeout)))
        {
            LogError("Failure setting option OPTION_EVENT_SEND_TIMEOUT_SECS");
            result++;
        }
    }
    else if (protocol_type == TEST_HTTP)
    {
        unsigned int min_poll_time = 5 * 50;
        bool batching = true;
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_MIN_POLLING_TIME, &min_poll_time)))
        {
            LogError("Failure setting option OPTION_MIN_POLLING_TIME");
            result++;
        }
        if (!check_iothub_result(IoTHubDeviceClient_LL_SetOption(device_client, OPTION_BATCHING, &batching)))
        {
            LogError("Failure setting option OPTION_BATCHING");
            result++;
        }
    }
    return result;
}

static int send_telemetry_message(IOTHUB_DEVICE_CLIENT_LL_HANDLE device_client, MESSAGE_CREATION_MECHANISM msg_type, void* context)
{
    int result = 0;
    // Create the message
    IOTHUB_MESSAGE_HANDLE event_msg;
    if (msg_type == TEST_MESSAGE_CREATE_BYTE_ARRAY)
    {
        event_msg = IoTHubMessage_CreateFromByteArray(TEST_BACK_COMPAT_MSG_BYTE, TEST_MSG_BYTE_SIZE);
    }
    else if (msg_type == TEST_MESSAGE_CREATE_STRING)
    {
        event_msg = IoTHubMessage_CreateFromString(TEST_BACK_COMPAT_MSG_STRING);
    }

    if (event_msg == NULL)
    {
        LogError("Failure creating Iothub message");
        result = __LINE__;
    }
    else
    {
        const char* test_value;

        IOTHUBMESSAGE_CONTENT_TYPE content_type = IoTHubMessage_GetContentType(event_msg);
        if (content_type == IOTHUBMESSAGE_BYTEARRAY)
        {
            const unsigned char* buffer;
            size_t size;
            if (IoTHubMessage_GetByteArray(event_msg, &buffer, &size) != IOTHUB_MESSAGE_OK)
            {
                LogError("Failure Getting byte array from iothub message");
                result = __LINE__;
            }
        }
        else if (content_type == IOTHUBMESSAGE_STRING)
        {
            const char* message = IoTHubMessage_GetString(event_msg);
            if (message == NULL)
            {
                LogError("Failure Getting string from iothub message");
                result = __LINE__;
            }
        }

        // Enter properties
        (void)IoTHubMessage_SetMessageId(event_msg, "MSG_ID");
        test_value = IoTHubMessage_GetMessageId(event_msg);

        (void)IoTHubMessage_SetCorrelationId(event_msg, "CORE_ID");
        test_value = IoTHubMessage_GetCorrelationId(event_msg);

        (void)IoTHubMessage_SetContentTypeSystemProperty(event_msg, "application%2fjson");
        test_value = IoTHubMessage_GetContentTypeSystemProperty(event_msg);

        (void)IoTHubMessage_SetContentEncodingSystemProperty(event_msg, "utf-8");
        test_value = IoTHubMessage_GetContentEncodingSystemProperty(event_msg);

        (void)IoTHubMessage_SetOutputName(event_msg, "output_name");
        test_value = IoTHubMessage_GetOutputName(event_msg);

        (void)IoTHubMessage_SetInputName(event_msg, "input_name");
        test_value = IoTHubMessage_GetInputName(event_msg);

        // Add custom properties to message
        (void)IoTHubMessage_SetProperty(event_msg, "property_key", "property_value");
        test_value = IoTHubMessage_GetProperty(event_msg, "property_key");

        if (IoTHubDeviceClient_LL_SendEventAsync(device_client, event_msg, message_recv_callback, context) != IOTHUB_CLIENT_OK)
        {
            LogError("Failure calling send event async");
            result = __LINE__;
        }
    }
    return result;
}

static int send_command_info(MIN_E2E_TEST_INFO* min_e2e_info, MESSAGE_CREATION_MECHANISM msg_type)
{
    int result;
    CMD_CHANNEL_CTX cmd_channel;
    
    IOTHUB_MESSAGE_HANDLE event_msg;

    // Construct ID
    event_msg = IoTHubMessage_CreateFromString(min_e2e_info->msg_id);

    char telemetry_count[16];
    sprintf(telemetry_count, "%d", min_e2e_info->d2c_msg_count);
    (void)IoTHubMessage_SetProperty(event_msg, "telemetry_count", telemetry_count);


}

MIN_E2E_TEST_HANDLE min_e2e_create(TEST_PROTOCOL_TYPE protocol_type)
{
    MIN_E2E_TEST_INFO* result;
    if ((result = (MIN_E2E_TEST_INFO*)malloc(sizeof(MIN_E2E_TEST_INFO))) == NULL)
    {
        LogError("Failure allocating e2e object");
    }
    else
    {
        memset(result, 0, sizeof(MIN_E2E_TEST_INFO));
        if ((result->tick_cntr_handle = tickcounter_create()) == NULL)
        {
            LogError("Failure creating tickcounter object");
            free(result);
            result = NULL;
        }
        else
        {
            result->connection_string = environment_get_variable("IOTHUB_DEVICE_CONN_STRING");
            result->protocol = protocol_type;
            strcpy(result->msg_id, "123456789");
            result->d2c_msg_count = 5;
        }
    }
    return result;
}

void min_e2e_destroy(MIN_E2E_TEST_HANDLE handle)
{
    if (handle != NULL)
    {
        tickcounter_destroy(handle->tick_cntr_handle);
        free(handle);
    }
}

int min_e2e_open_ctrl_channel(MIN_E2E_TEST_HANDLE handle, DEVICE_CREATION_TYPE type, MIN_E2E_TEST_TYPE test_type, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    int result;
    (void)test_type;
    MIN_E2E_MSG_CTX min_e2e_ctx = { 0 };

    if ((handle->device_handle = create_device_client(handle, type, protocol, &min_e2e_ctx)) == NULL)
    {
        LogError("Failure creating tickcounter object");
        result = __LINE__;
    }
    else
    {
        tickcounter_ms_t initial_time;
        // Wait to be connected
        tickcounter_get_current_ms(handle->tick_cntr_handle, &initial_time);
        do
        {
            IoTHubDeviceClient_LL_DoWork(handle->device_handle);
        } while (!min_e2e_ctx.connected && !min_e2e_ctx.is_error && !is_operation_timed_out(handle->tick_cntr_handle, initial_time, MAX_OPERATION_TIMEOUT));

        if (!min_e2e_ctx.connected || min_e2e_ctx.is_error)
        {
            LogError("Failure connecting to iothub service");
            result = __LINE__;
        }
        else
        {
            result = 0;
        }
    }
    return 0;
}

void min_e2e_close_ctrl_channel(MIN_E2E_TEST_HANDLE handle)
{
    IoTHubDeviceClient_LL_Destroy(handle->device_handle);
}

int min_e2e_execute_telemetry_tests(MIN_E2E_TEST_HANDLE handle, size_t message_count, MESSAGE_CREATION_MECHANISM msg_create_type)
{
    (void)handle;
    (void)message_count;
    (void)msg_create_type;
    return 0;
}

int min_e2e_execute_method_tests(MIN_E2E_TEST_HANDLE handle)
{
    (void)handle;
    return 0;
}

int min_e2e_execute_twin_tests(MIN_E2E_TEST_HANDLE handle)
{
    (void)handle;
    return 0;
}

/******************************************************************************
* File Name:   secure_tcp_client.c
*
* Description: This file contains task and functions related to secure TCP
* client operation.
*
*******************************************************************************
* (c) 2019-2020, Cypress Semiconductor Corporation. All rights reserved.
*******************************************************************************
* This software, including source code, documentation and related materials
* ("Software"), is owned by Cypress Semiconductor Corporation or one of its
* subsidiaries ("Cypress") and is protected by and subject to worldwide patent
* protection (United States and foreign), United States copyright laws and
* international treaty provisions. Therefore, you may use this Software only
* as provided in the license agreement accompanying the software package from
* which you obtained this Software ("EULA").
*
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software source
* code solely for use in connection with Cypress's integrated circuit products.
* Any reproduction, modification, translation, compilation, or representation
* of this Software except as specified above is prohibited without the express
* written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer of such
* system or application assumes all risk of such use and in doing so agrees to
* indemnify Cypress against all liability.
*******************************************************************************/

/* Header file includes. */
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

/* FreeRTOS header file. */
#include <FreeRTOS.h>
#include <task.h>

/* Standard C header file. */
#include <string.h>

/* Cypress secure socket header file. */
#include "cy_secure_sockets.h"
#include "cy_tls.h"

/* Wi-Fi connection manager header files. */
#include "cy_wcm.h"
#include "cy_wcm_error.h"

#include "network_credentials.h"

/* Secure TCP client task header file. */
#include "secure_tcp_client.h"

/******************************************************************************
* Macros
******************************************************************************/
/* RTOS related macros. */
#define RTOS_TASK_TICKS_TO_WAIT           (1000)

#define LED_ON_CMD                         '1'
#define LED_OFF_CMD                        '0'
#define ACK_LED_ON                         "LED ON ACK"
#define ACK_LED_OFF                        "LED OFF ACK"
#define MSG_INVALID_CMD                    "Invalid command"

/******************************************************************************
* Function Prototypes
******************************************************************************/
cy_rslt_t connect_to_wifi_ap(void);
cy_rslt_t connect_to_secure_tcp_server(cy_socket_sockaddr_t address);
cy_rslt_t create_secure_tcp_client_socket();
cy_rslt_t tcp_client_recv_handler(cy_socket_t socket_handle, void *arg);
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg);

/******************************************************************************
* Global Variables
******************************************************************************/

/* TLS credentials of the TCP client. */
static const char tcp_client_cert[] = keyCLIENT_CERTIFICATE_PEM;
static const char client_private_key[] = keyCLIENT_PRIVATE_KEY_PEM;

/* Root CA certificate for TCP server identity verification. */
static const char tcp_server_ca_cert[] = keySERVER_ROOTCA_PEM;

/* Variable to store the TLS identity (certificate and private key).*/
void *tls_identity;

/* TCP client socket handle */
cy_socket_t client_handle;

/* Variable to check if a TCP server is connected. */
bool server_connected = false;

/*******************************************************************************
 * Function Name: tcp_secure_client_task
 *******************************************************************************
 * Summary:
 *  Task used to establish a secure connection to a remote TCP server and
 *  control the LED state (ON/OFF) based on the command received from TCP server.
 *
 * Parameters:
 *  void *args : Task parameter defined during task creation (unused)
 *
 * Return:
 *  void
 *
 *******************************************************************************/
void tcp_secure_client_task(void *arg)
{
    cy_rslt_t result;

    /* Connect to Wi-Fi AP */
    if(connect_to_wifi_ap() != CY_RSLT_SUCCESS )
    {
        printf("\n Failed to connect to Wi-FI AP.\n");
        CY_ASSERT(0);
    }

    /* IP address and TCP port number of the TCP server to which the TCP client
     * connects to. */
    cy_socket_sockaddr_t tcp_server_address = {
            .ip_address.ip.v4 = TCP_SERVER_IP_ADDRESS,
            .ip_address.version = CY_SOCKET_IP_VER_V4,
            .port = TCP_SERVER_PORT
    };

    /* TCP client certificate length and private key length. */
    const size_t tcp_client_cert_len = strlen( tcp_client_cert );
    const size_t pkey_len = strlen( client_private_key );

    /* Initialize secure socket library. */
    result = cy_socket_init();
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Secure Socket initialization failed!\n");
        CY_ASSERT(0);
    }
    printf("Secure Socket initialized\n");

    /* Initializes the global trusted RootCA certificate. This examples uses a self signed
     * certificate which implies that the RootCA certificate is same as the certificate of
     * TCP secure server to which client is connecting to.
     */
    result = cy_tls_load_global_root_ca_certificates(tcp_server_ca_cert, strlen(tcp_server_ca_cert));
    if( result != CY_RSLT_SUCCESS)
    {
        printf("cy_tls_load_global_root_ca_certificates failed\n");
    }
    else
    {
        printf("Global trusted RootCA certificate loaded\n");
    }

    /* Create TCP client identity using the SSL certificate and private key. */
    result = cy_tls_create_identity(tcp_client_cert, tcp_client_cert_len,
                                    client_private_key, pkey_len, &tls_identity);
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed cy_tls_create_identity! Error code: %d\n", (int)result);
        CY_ASSERT(0);
    }   

    for(;;)
    {        
        if(server_connected == false)
        {       
            /* Connect to the secure TCP server. If the connection fails, retry
             * to connect to the server for MAX_TCP_SERVER_CONN_RETRIES times. */
            printf("Connecting to TCP Server...\n");
            result = connect_to_secure_tcp_server(tcp_server_address);

            if(result != CY_RSLT_SUCCESS)
            {
                printf("Failed to connect to TCP server.\n");
                CY_ASSERT(0);
            }
        }

        vTaskDelay(RTOS_TASK_TICKS_TO_WAIT);
    }
 }

/*******************************************************************************
 * Function Name: connect_to_wifi_ap()
 *******************************************************************************
 * Summary:
 *  Connects to Wi-Fi AP using the user-configured credentials, retries up to a
 *  configured number of times until the connection succeeds.
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t connect_to_wifi_ap(void)
{
    cy_rslt_t result;

    /* Variables used by Wi-Fi connection manager.*/
    cy_wcm_connect_params_t wifi_conn_param;

    cy_wcm_config_t wifi_config = {
            .interface = CY_WCM_INTERFACE_TYPE_STA
    };

    cy_wcm_ip_address_t ip_address;

     /* Initialize Wi-Fi connection manager. */
    result = cy_wcm_init(&wifi_config);

    if (result != CY_RSLT_SUCCESS)
    {
        printf("Wi-Fi Connection Manager initialization failed!\n");
        return result;
    }
    printf("Wi-Fi Connection Manager initialized.\r\n");

     /* Set the Wi-Fi SSID, password and security type. */
    memset(&wifi_conn_param, 0, sizeof(cy_wcm_connect_params_t));
    memcpy(wifi_conn_param.ap_credentials.SSID, WIFI_SSID, sizeof(WIFI_SSID));
    memcpy(wifi_conn_param.ap_credentials.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
    wifi_conn_param.ap_credentials.security = WIFI_SECURITY_TYPE;

    /* Join the Wi-Fi AP. */
    for(uint32_t conn_retries = 0; conn_retries < MAX_WIFI_CONN_RETRIES; conn_retries++ )
    {
        result = cy_wcm_connect_ap(&wifi_conn_param, &ip_address);

        if(result == CY_RSLT_SUCCESS)
        {
            printf("Successfully connected to Wi-Fi network '%s'.\n",
                                wifi_conn_param.ap_credentials.SSID);
            printf("IP Address Assigned: %d.%d.%d.%d\n", (uint8_t)ip_address.ip.v4,
                    (uint8_t)(ip_address.ip.v4 >> 8), (uint8_t)(ip_address.ip.v4 >> 16),
                    (uint8_t)(ip_address.ip.v4 >> 24));
            return result;
        }

        printf("Connection to Wi-Fi network failed with error code %d."
               "Retrying in %d ms...\n", (int)result, WIFI_CONN_RETRY_INTERVAL_MSEC);

        vTaskDelay(pdMS_TO_TICKS(WIFI_CONN_RETRY_INTERVAL_MSEC));
    }

    /* Stop retrying after maximum retry attempts. */
    printf("Exceeded maximum Wi-Fi connection attempts\n");

    return result;
}

/*******************************************************************************
 * Function Name: create_secure_tcp_client_socket
 *******************************************************************************
 * Summary:
 *  Function to create a secure socket and set the socket options to use TLS
 *  identity, set call back function for handling incoming messages, call back
 *  function to handle disconnection.
 *
 * Return:
 *  cy_result result: Result of the operation.
 *
 *******************************************************************************/
cy_rslt_t create_secure_tcp_client_socket()
{
    cy_rslt_t result;

    /* Variables used to set socket options. */
    cy_socket_opt_callback_t tcp_recv_option;
    cy_socket_opt_callback_t tcp_disconnection_option;

    /* Create a new secure TCP socket. */
    result = cy_socket_create(CY_SOCKET_DOMAIN_AF_INET, CY_SOCKET_TYPE_STREAM,
                              CY_SOCKET_IPPROTO_TLS, &client_handle);

    if (result != CY_RSLT_SUCCESS)
    {
        printf("Failed to create socket!\n");
        return result;
    }

    /* Register the callback function to handle messages received from TCP server. */
    tcp_recv_option.callback = tcp_client_recv_handler;
    tcp_recv_option.arg = NULL;
    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_RECEIVE_CALLBACK,
                                  &tcp_recv_option, sizeof(cy_socket_opt_callback_t));
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_RECEIVE_CALLBACK failed\n");
        return result;
    }

    /* Register the callback function to handle disconnection. */
    tcp_disconnection_option.callback = tcp_disconnection_handler;
    tcp_disconnection_option.arg = NULL;

    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_SOCKET,
                                  CY_SOCKET_SO_DISCONNECT_CALLBACK,
                                  &tcp_disconnection_option, sizeof(cy_socket_opt_callback_t));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Set socket option: CY_SOCKET_SO_DISCONNECT_CALLBACK failed\n");
        return result;
    }

    /* Set the TCP socket to use the TLS identity. */
    result = cy_socket_setsockopt(client_handle, CY_SOCKET_SOL_TLS, CY_SOCKET_SO_TLS_IDENTITY,
                                  tls_identity, sizeof(tls_identity));
    if(result != CY_RSLT_SUCCESS)
    {
        printf("Failed cy_socket_setsockopt! Error code: %d\n", (int)result);
    }

    return result;
}

/*******************************************************************************
 * Function Name: connect_to_secure_tcp_server
 *******************************************************************************
 * Summary:
 *  Function to connect to secure TCP server.
 *
 * Parameters:
 *  cy_socket_sockaddr_t address: Address of TCP server socket
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t connect_to_secure_tcp_server(cy_socket_sockaddr_t address)
{
    cy_rslt_t result = CY_RSLT_MODULE_SECURE_SOCKETS_TIMEOUT;
    cy_rslt_t conn_result;  

    for(uint32_t conn_retries = 0; conn_retries < MAX_TCP_SERVER_CONN_RETRIES; conn_retries++)
    {
        /* Create a secure TCP socket */
        conn_result = create_secure_tcp_client_socket();
        if(conn_result != CY_RSLT_SUCCESS)
        {
            printf("Failed to create secure socket!\n");
            CY_ASSERT(0);
        }
        
        conn_result = cy_socket_connect(client_handle, &address, sizeof(cy_socket_sockaddr_t));
        if (conn_result == CY_RSLT_SUCCESS)
        {
            printf("============================================================\n");
            printf("TLS Handshake successful and connected to TCP server\n");
            server_connected = true;
            return conn_result;
        }

        printf("Could not connect to TCP Server.\n");
        printf("Trying to reconnect to TCP Server...Please check if server is listening\n");

        /* The resources allocated during the socket creation (cy_socket_create)
         * should be deleted.
         */
        cy_socket_delete(client_handle);
    }

     /* Stop retrying after maximum retry attempts. */
     printf("Exceeded maximum connection attempts to the TCP server\n");

     return result;
}

/*******************************************************************************
 * Function Name: tcp_client_recv_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle incoming TCP server messages.
 *
 * Parameters:
 *  cy_socket_t socket_handle: Connection handle for the TCP client socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_client_recv_handler(cy_socket_t socket_handle, void *arg)
{
    /* Variable to store number of bytes send to the TCP server. */
    uint32_t bytes_sent = 0;

    /* Variable to store number of bytes received. */
    uint32_t bytes_received = 0;

    char message_buffer[MAX_TCP_DATA_PACKET_LENGTH];
    cy_rslt_t result ;

    printf("============================================================\n");
    result = cy_socket_recv(socket_handle, message_buffer, TCP_LED_CMD_LEN,
                            CY_SOCKET_FLAGS_NONE, &bytes_received);

    if(message_buffer[0] == LED_ON_CMD)
    {
        /* Turn the LED ON. */
        cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_ON);
        printf("LED turned ON\n");
        sprintf(message_buffer, ACK_LED_ON);
    }
    else if(message_buffer[0] == LED_OFF_CMD)
    {
        /* Turn the LED OFF. */
        cyhal_gpio_write(CYBSP_USER_LED, CYBSP_LED_STATE_OFF);
        printf("LED turned OFF\n");
        sprintf(message_buffer, ACK_LED_OFF);
    }
    else
    {
        printf("Invalid command\n");
        sprintf(message_buffer, MSG_INVALID_CMD);
    }

    /* Send acknowledgement to the secure TCP server in receipt of the message received. */
    result = cy_socket_send(socket_handle, message_buffer, strlen(message_buffer),
                            CY_SOCKET_FLAGS_NONE, &bytes_sent);
    if(result == CY_RSLT_SUCCESS)
    {
        printf("Acknowledgement sent to TCP server\n");
    }

    return result;
}

/*******************************************************************************
 * Function Name: tcp_disconnection_handler
 *******************************************************************************
 * Summary:
 *  Callback function to handle TCP client disconnection event.
 *
 * Parameters:
 * cy_socket_t socket_handle: Connection handle for the TCP server socket
 *  void *args : Parameter passed on to the function (unused)
 *
 * Return:
 *  cy_result result: Result of the operation
 *
 *******************************************************************************/
cy_rslt_t tcp_disconnection_handler(cy_socket_t socket_handle, void *arg)
{
    cy_rslt_t result;

    /* Disconnect the TCP client. */
    result = cy_socket_disconnect(socket_handle, 0);
    
    /* Free the resources allocated to the socket. */
    cy_socket_delete(socket_handle);

    /* Set the client connection flag as false. */
    server_connected = false;
    printf("Disconnected from the TCP server! \n");

    return result;
}

/* [] END OF FILE */
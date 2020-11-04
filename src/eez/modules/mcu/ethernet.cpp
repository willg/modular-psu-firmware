/*
 * EEZ Middleware
 * Copyright (C) 2015-present, Envox d.o.o.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#if OPTION_ETHERNET

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <memory.h>

#if defined(EEZ_PLATFORM_STM32)
#include <lwip.h>
#include <api.h>
#include <dhcp.h>
#include <ip_addr.h>
#include <netif.h>
#include <ethernetif.h>
extern struct netif gnetif;
extern ip4_addr_t ipaddr;
extern ip4_addr_t netmask;
extern ip4_addr_t gw;
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
#undef INPUT
#undef OUTPUT
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winsock2.h>
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#endif

#include <eez/firmware.h>
#include <eez/system.h>
#include <eez/modules/mcu/ethernet.h>
#include <eez/modules/psu/psu.h>
#include <eez/modules/psu/ethernet.h>
#include <eez/modules/psu/persist_conf.h>

#include <eez/mqtt.h>
#include <eez/modules/psu/ntp.h>

using namespace eez::psu::ethernet;
using namespace eez::scpi;

#define CONF_CONNECT_TIMEOUT 30000

namespace eez {
namespace mcu {
namespace ethernet {

static void mainLoop(const void *);

#if defined(EEZ_PLATFORM_STM32)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#endif

osThreadDef(g_ethernetTask, mainLoop, osPriorityNormal, 0, 1024);

#if defined(EEZ_PLATFORM_STM32)
#pragma GCC diagnostic pop
#endif

osMessageQDef(g_ethernetMessageQueue, 20, uint32_t);
osMessageQId(g_ethernetMessageQueueId);

static osThreadId g_ethernetTaskHandle;

void initMessageQueue() {
    g_ethernetMessageQueueId = osMessageCreate(osMessageQ(g_ethernetMessageQueue), NULL);
}

void startThread() {
    g_ethernetTaskHandle = osThreadCreate(osThread(g_ethernetTask), nullptr);
}

enum {
	QUEUE_MESSAGE_CONNECT,
	QUEUE_MESSAGE_CREATE_TCP_SERVER,
    QUEUE_MESSAGE_DESTROY_TCP_SERVER,
	QUEUE_MESSAGE_ACCEPT_CLIENT,
	QUEUE_MESSAGE_CLIENT_MESSAGE,
    QUEUE_MESSAGE_PUSH_EVENT,
    QUEUE_MESSAGE_NTP_STATE_TRANSITION
};

#if defined(EEZ_PLATFORM_STM32)
enum ConnectionState {
    CONNECTION_STATE_INITIALIZED,
    CONNECTION_STATE_CONNECTED,
    CONNECTION_STATE_CONNECTING,
    CONNECTION_STATE_CONNECT_ERROR,
    CONNECTION_STATE_BEGIN_SERVER,
    CONNECTION_STATE_CLIENT_AVAILABLE,
    CONNECTION_STATE_DATA_AVAILABLE
};

static ConnectionState g_connectionState = CONNECTION_STATE_INITIALIZED;
static uint16_t g_port;
struct netconn *g_tcpListenConnection;
struct netconn *g_tcpClientConnection;
static netbuf *g_inbuf;
static bool g_checkLinkWhileIdle = false;
static bool g_acceptClientIsDone;

static void netconnCallback(struct netconn *conn, enum netconn_evt evt, u16_t len) {
	switch (evt) {
	case NETCONN_EVT_RCVPLUS:
		if (conn == g_tcpListenConnection) {
            g_acceptClientIsDone = false;
			osMessagePut(g_ethernetMessageQueueId, QUEUE_MESSAGE_ACCEPT_CLIENT, osWaitForever);
            while (!g_acceptClientIsDone) {
                osDelay(1);
            }
		} else if (conn == g_tcpClientConnection) {
			sendMessageToLowPriorityThread(ETHERNET_INPUT_AVAILABLE);
		}
		break;

	case NETCONN_EVT_RCVMINUS:
		osDelay(0);
		break;

	case NETCONN_EVT_SENDPLUS:
		osDelay(0);
		break;

	case NETCONN_EVT_SENDMINUS:
		osDelay(0);
		break;

	case NETCONN_EVT_ERROR:
        // DebugTrace("NETCONN_EVT_ERROR\n");
		osDelay(0);
		break;
	}
}

static void dhcpStart() {
    uint32_t connectStart = millis();

    if (psu::persist_conf::isEthernetDhcpEnabled()) {
        // Start DHCP negotiation for a network interface (IPv4)
        dhcp_start(&gnetif);
        while(!dhcp_supplied_address(&gnetif)) {
            if ((millis() - connectStart) >= CONF_CONNECT_TIMEOUT) {
                g_connectionState = CONNECTION_STATE_CONNECT_ERROR;
                sendMessageToLowPriorityThread(ETHERNET_CONNECTED);
                return;
            }
            osDelay(10);
        }
    }

    g_connectionState = CONNECTION_STATE_CONNECTED;
    sendMessageToLowPriorityThread(ETHERNET_CONNECTED, 1);
    return;
}

static void onEvent(uint8_t eventType) {
	switch (eventType) {
	case QUEUE_MESSAGE_CONNECT:
		{
            g_checkLinkWhileIdle = true;

            // MX_LWIP_Init();

            // Initilialize the LwIP stack with RTOS
            tcpip_init(NULL, NULL);

            if (psu::persist_conf::isEthernetDhcpEnabled()) {            
                ipaddr.addr = 0;
                netmask.addr = 0;
                gw.addr = 0;
            } else {
            	ipaddr.addr = psu::persist_conf::devConf.ethernetIpAddress;
            	netmask.addr = psu::persist_conf::devConf.ethernetSubnetMask;
            	gw.addr = psu::persist_conf::devConf.ethernetGateway;
            }

            // add the network interface (IPv4/IPv6) with RTOS
            netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &tcpip_input);

			netif_set_hostname(&gnetif, psu::persist_conf::devConf.ethernetHostName);

            // Registers the default network interface
            netif_set_default(&gnetif);

            if (netif_is_link_up(&gnetif)) {
                // When the netif is fully configured this function must be called
                netif_set_up(&gnetif);
                dhcpStart();
            } else {
                // When the netif link is down this function must be called
                netif_set_down(&gnetif);
                g_connectionState = CONNECTION_STATE_CONNECT_ERROR;
                sendMessageToLowPriorityThread(ETHERNET_CONNECTED);
            }

		}
		break;

	case QUEUE_MESSAGE_CREATE_TCP_SERVER:
		g_tcpListenConnection = netconn_new_with_callback(NETCONN_TCP, netconnCallback);
		if (g_tcpListenConnection == nullptr) {
			break;
		}

		// Is this required?
		// netconn_set_nonblocking(conn, 1);

		if (netconn_bind(g_tcpListenConnection, nullptr, g_port) != ERR_OK) {
			netconn_delete(g_tcpListenConnection);
			break;
		}

		netconn_listen(g_tcpListenConnection);
		break;

    case QUEUE_MESSAGE_DESTROY_TCP_SERVER:
        netconn_delete(g_tcpListenConnection);
        g_tcpListenConnection = nullptr;
        break;

	case QUEUE_MESSAGE_ACCEPT_CLIENT:
		{
			struct netconn *newConnection;
			if (netconn_accept(g_tcpListenConnection, &newConnection) == ERR_OK) {
				if (g_tcpClientConnection) {
					// there is a client already connected, close this connection
                    g_acceptClientIsDone = true;
                    osDelay(10);
					netconn_delete(newConnection);
				} else {
					// connection with the client established
					g_tcpClientConnection = newConnection;
					sendMessageToLowPriorityThread(ETHERNET_CLIENT_CONNECTED);
                    g_acceptClientIsDone = true;
				}
			}  else {
                g_acceptClientIsDone = true;
            }
		}
		break;
	}
}

void onIdle() {
	if (g_checkLinkWhileIdle) {
		uint32_t regvalue = 0;

		/* Read PHY_BSR*/
		HAL_ETH_ReadPHYRegister(&heth, PHY_BSR, &regvalue);

		regvalue &= PHY_LINKED_STATUS;

		/* Check whether the netif link down and the PHY link is up */
		if(!netif_is_link_up(&gnetif) && (regvalue)) {
			/* network cable is connected */
			netif_set_up(&gnetif);
			netif_set_link_up(&gnetif);
            dhcpStart();
		} else if(netif_is_link_up(&gnetif) && (!regvalue)) {
			/* network cable is dis-connected */
			netif_set_link_down(&gnetif);
            g_connectionState = CONNECTION_STATE_CONNECT_ERROR;
            sendMessageToLowPriorityThread(ETHERNET_CONNECTED);
		}
	}

	if (g_tcpClientConnection) {

	}
}
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
#define INPUT_BUFFER_SIZE 1024

static uint16_t g_port;
static char g_inputBuffer[INPUT_BUFFER_SIZE];
static uint32_t g_inputBufferLength;

////////////////////////////////////////////////////////////////////////////////

bool bind(int port);
bool client_available();
bool connected();
int available();
int read(char *buffer, int buffer_size);
int write(const char *buffer, int buffer_size);
void stop();

#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
static SOCKET listen_socket = INVALID_SOCKET;
static SOCKET client_socket = INVALID_SOCKET;
#else
static int listen_socket = -1;
static int client_socket = -1;

bool enable_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    flags = flags | O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
        return false;
    }
    return true;
}

#endif

bool bind(int port) {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    WSADATA wsaData;
    int iResult;

    struct addrinfo *result = NULL;
    struct addrinfo hints;

    // Initialize Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        DebugTrace("EHTERNET: WSAStartup failed with error %d\n", iResult);
        return false;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    // Resolve the server address and port
    char port_str[16];
#ifdef _MSC_VER
    _itoa(port, port_str, 10);
#else
    itoa(port, port_str, 10);
#endif
    iResult = getaddrinfo(NULL, port_str, &hints, &result);
    if (iResult != 0) {
        DebugTrace("EHTERNET: getaddrinfo failed with error %d\n", iResult);
        return false;
    }

    // Create a SOCKET for connecting to server
    listen_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (listen_socket == INVALID_SOCKET) {
        DebugTrace("EHTERNET: socket failed with error %d\n", WSAGetLastError());
        freeaddrinfo(result);
        return false;
    }

    u_long iMode = 1;
    iResult = ioctlsocket(listen_socket, FIONBIO, &iMode);
    if (iResult != NO_ERROR) {
        DebugTrace("EHTERNET: ioctlsocket failed with error %d\n", iResult);
        freeaddrinfo(result);
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
        return false;
    }

    // Setup the TCP listening socket
    iResult = ::bind(listen_socket, result->ai_addr, (int)result->ai_addrlen);
    if (iResult == SOCKET_ERROR) {
        DebugTrace("EHTERNET: bind failed with error %d\n", WSAGetLastError());
        freeaddrinfo(result);
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
        return false;
    }

    freeaddrinfo(result);

    iResult = listen(listen_socket, SOMAXCONN);
    if (iResult == SOCKET_ERROR) {
        DebugTrace("EHTERNET listen failed with error %d\n", WSAGetLastError());
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
        return false;
    }

    return true;
#else
    sockaddr_in serv_addr;
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        DebugTrace("EHTERNET: socket failed with error %d", errno);
        return false;
    }

    if (!enable_non_blocking(listen_socket)) {
        DebugTrace("EHTERNET: ioctl on listen socket failed with error %d", errno);
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    bzero((char *)&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (::bind(listen_socket, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        DebugTrace("EHTERNET: bind failed with error %d", errno);
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    if (listen(listen_socket, 5) < 0) {
        DebugTrace("EHTERNET: listen failed with error %d", errno);
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    return true;
#endif    
}

bool client_available() {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    if (connected())
        return true;

    if (listen_socket == INVALID_SOCKET) {
        return false;
    }

    // Accept a client socket
    client_socket = accept(listen_socket, NULL, NULL);
    if (client_socket == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return false;
        }

        DebugTrace("EHTERNET accept failed with error %d\n", WSAGetLastError());
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
        return false;
    }

    return true;
#else
    if (connected())
        return true;

    if (listen_socket == -1) {
        return 0;
    }

    sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    client_socket = accept(listen_socket, (sockaddr *)&cli_addr, &clilen);
    if (client_socket < 0) {
        if (errno == EWOULDBLOCK) {
            return false;
        }

        DebugTrace("EHTERNET: accept failed with error %d", errno);
        close(listen_socket);
        listen_socket = -1;
        return false;
    }

    if (!enable_non_blocking(client_socket)) {
        DebugTrace("EHTERNET: ioctl on client socket failed with error %d", errno);
        close(client_socket);
        listen_socket = -1;
        return false;
    }

    return true;
#endif    
}

bool connected() {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    return client_socket != INVALID_SOCKET;
#else
    return client_socket != -1;
#endif    
}

int available() {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    if (client_socket == INVALID_SOCKET)
        return 0;

    int iResult = ::recv(client_socket, g_inputBuffer, INPUT_BUFFER_SIZE, MSG_PEEK);
    if (iResult > 0) {
        return iResult;
    }

    if (iResult < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        return 0;
    }

    stop();

    return 0;
#else
    if (client_socket == -1)
        return 0;

    char buffer[1000];
    int iResult = ::recv(client_socket, buffer, 1000, MSG_PEEK);
    if (iResult > 0) {
        return iResult;
    }

    if (iResult < 0 && errno == EWOULDBLOCK) {
        return 0;
    }

    stop();

    return 0;
#endif        
}

int read(char *buffer, int buffer_size) {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    int iResult = ::recv(client_socket, buffer, buffer_size, 0);
    if (iResult > 0) {
        return iResult;
    }

    if (iResult < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        return 0;
    }

    stop();

    return 0;
#else
    int n = ::read(client_socket, buffer, buffer_size);
    if (n > 0) {
        return n;
    }

    if (n < 0 && errno == EWOULDBLOCK) {
        return 0;
    }

    stop();

    return 0;
#endif    
}

int write(const char *buffer, int buffer_size) {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    int iSendResult;

    if (client_socket != INVALID_SOCKET) {
        // Echo the buffer back to the sender
        iSendResult = ::send(client_socket, buffer, buffer_size, 0);
        if (iSendResult == SOCKET_ERROR) {
            DebugTrace("send failed with error: %d\n", WSAGetLastError());
            closesocket(client_socket);
            client_socket = INVALID_SOCKET;
            return 0;
        }
        return iSendResult;
    }

    return 0;
#else
    if (client_socket != -1) {
        int n = ::write(client_socket, buffer, buffer_size);
        if (n < 0) {
            close(client_socket);
            client_socket = -1;
            return 0;
        }
        return n;
    }

    return 0;
#endif    
}

void stop() {
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
    if (client_socket != INVALID_SOCKET) {
        int iResult = ::shutdown(client_socket, SD_SEND);
        if (iResult == SOCKET_ERROR) {
            DebugTrace("EHTERNET shutdown failed with error %d\n", WSAGetLastError());
        }
        closesocket(client_socket);
        client_socket = INVALID_SOCKET;
    }
#else
    int result = ::shutdown(client_socket, SHUT_WR);
    if (result < 0) {
        DebugTrace("ETHERNET shutdown failed with error %d\n", errno);
    }
    close(client_socket);
    client_socket = -1;
#endif    
}

void onEvent(uint8_t eventType) {
    switch (eventType) {
    case QUEUE_MESSAGE_CONNECT:
        sendMessageToLowPriorityThread(ETHERNET_CONNECTED, 1);
        break;

    case QUEUE_MESSAGE_CREATE_TCP_SERVER:
        bind(g_port);
        break;

    case QUEUE_MESSAGE_DESTROY_TCP_SERVER:
#ifdef EEZ_PLATFORM_SIMULATOR_WIN32
        if (listen_socket != INVALID_SOCKET) {
            closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
#else
        close(listen_socket);
        listen_socket = -1;
#endif    
        break;
    }
}

void onIdle() {
    static bool wasConnected = false;

    if (wasConnected) {
        if (connected()) {
            if (!g_inputBufferLength && available()) {
                g_inputBufferLength = read(g_inputBuffer, INPUT_BUFFER_SIZE);
                sendMessageToLowPriorityThread(ETHERNET_INPUT_AVAILABLE);
            }
        } else {
            sendMessageToLowPriorityThread(ETHERNET_CLIENT_DISCONNECTED);
            wasConnected = false;
        }
    } else {
        if (client_available()) {
            wasConnected = true;
            sendMessageToLowPriorityThread(ETHERNET_CLIENT_CONNECTED);
        }
    }
}
#endif

void mainLoop(const void *) {
    while (1) {
        osEvent event = osMessageGet(g_ethernetMessageQueueId, 10);
        if (event.status == osEventMessage) {
            uint8_t eventType = event.value.v & 0xFF;
            if (eventType == QUEUE_MESSAGE_PUSH_EVENT) {
                mqtt::pushEvent((int16_t)(event.value.v >> 8));
            } else if (eventType == QUEUE_MESSAGE_NTP_STATE_TRANSITION) {
                ntp::stateTransition(event.value.v >> 8);
            } else {
                onEvent(eventType);
            }
        } else {
            onIdle();
            mqtt::tick();
            ntp::tick();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void begin() {
#if defined(EEZ_PLATFORM_STM32)
	g_connectionState = CONNECTION_STATE_CONNECTING;
#endif
    osMessagePut(g_ethernetMessageQueueId, QUEUE_MESSAGE_CONNECT, osWaitForever);
}

IPAddress localIP() {
#if defined(EEZ_PLATFORM_STM32)
    return IPAddress(gnetif.ip_addr.addr);
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    return IPAddress();
#endif
}

IPAddress subnetMask() {
#if defined(EEZ_PLATFORM_STM32)
    return IPAddress(gnetif.netmask.addr);
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    return IPAddress();
#endif
}

IPAddress gatewayIP() {
#if defined(EEZ_PLATFORM_STM32)
    return IPAddress(gnetif.gw.addr);
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    return IPAddress();
#endif
}

IPAddress dnsServerIP() {
#if defined(EEZ_PLATFORM_STM32)
    return IPAddress();
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    return IPAddress();
#endif
}

void beginServer(uint16_t port) {
    g_port = port;
    osMessagePut(g_ethernetMessageQueueId, QUEUE_MESSAGE_CREATE_TCP_SERVER, osWaitForever);
}

void endServer() {
    osMessagePut(g_ethernetMessageQueueId, QUEUE_MESSAGE_DESTROY_TCP_SERVER, osWaitForever);
}

void getInputBuffer(int bufferPosition, char **buffer, uint32_t *length) {
#if defined(EEZ_PLATFORM_STM32)
	if (!g_tcpClientConnection) {
		return;
	}

	if (netconn_recv(g_tcpClientConnection, &g_inbuf) != ERR_OK) {
		goto fail1;
	}

	if (netconn_err(g_tcpClientConnection) != ERR_OK) {
		goto fail2;
	}

	uint8_t* data;
	u16_t dataLength;
	netbuf_data(g_inbuf, (void**)&data, &dataLength);

    if (dataLength > 0) {
    	*buffer = (char *)data;
    	*length = dataLength;
    } else {
        netbuf_delete(g_inbuf);
        g_inbuf = nullptr;
    	*buffer = nullptr;
    	*length = 0;
    }

    return;

fail2:
	netbuf_delete(g_inbuf);
	g_inbuf = nullptr;

fail1:
	netconn_delete(g_tcpClientConnection);
	g_tcpClientConnection = nullptr;
	sendMessageToLowPriorityThread(ETHERNET_CLIENT_DISCONNECTED);

	*buffer = nullptr;
	length = 0;
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    *buffer = g_inputBuffer;
    *length = g_inputBufferLength;
#endif
}

void releaseInputBuffer() {
#if defined(EEZ_PLATFORM_STM32)
	netbuf_delete(g_inbuf);
	g_inbuf = nullptr;
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    g_inputBufferLength = 0;
#endif
}

int writeBuffer(const char *buffer, uint32_t length) {
#if defined(EEZ_PLATFORM_STM32)
	netconn_write(g_tcpClientConnection, (void *)buffer, (uint16_t)length, NETCONN_COPY);
    return length;
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    int numWritten = write(buffer, length);
    osDelay(1);
    return numWritten;
#endif
}

void disconnectClient() {
#if defined(EEZ_PLATFORM_STM32)
	netconn_delete(g_tcpClientConnection);
	g_tcpClientConnection = nullptr;
#endif

#if defined(EEZ_PLATFORM_SIMULATOR)
    stop();
#endif    
}

void pushEvent(int16_t eventId) {
    if (!g_shutdownInProgress) {
        osMessagePut(g_ethernetMessageQueueId, ((uint32_t)(uint16_t)eventId << 8) | QUEUE_MESSAGE_PUSH_EVENT, 0);
    }
}

void ntpStateTransition(int transition) {
    if (!g_shutdownInProgress) {
        osMessagePut(g_ethernetMessageQueueId, (transition << 8) | QUEUE_MESSAGE_NTP_STATE_TRANSITION, 0);
    }
}

} // namespace ethernet
} // namespace mcu
} // namespace eez

#endif

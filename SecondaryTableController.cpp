/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#define LOG_TAG "SecondaryTablController"
#include <cutils/log.h>
#include <cutils/properties.h>

extern "C" int system_nosh(const char *command);

#include "ResponseCode.h"
#include "SecondaryTableController.h"

static char IP_PATH[] = "/system/bin/ip";
static char ADD[] = "add";
static char DEL[] = "del";

SecondaryTableController::SecondaryTableController() {
    int i;
    for (i=0; i < INTERFACES_TRACKED; i++) {
        mInterfaceTable[i][0] = 0;
        // TODO - use a hashtable or other prebuilt container class
        mInterfaceRuleCount[i] = 0;
    }
}

SecondaryTableController::~SecondaryTableController() {
}

int SecondaryTableController::findTableNumber(const char *iface) {
    int i;
    for (i = 0; i < INTERFACES_TRACKED; i++) {
        // compare through the final null, hence +1
        if (strncmp(iface, mInterfaceTable[i], IFNAMSIZ + 1) == 0) {
            return i;
        }
    }
    return -1;
}

int SecondaryTableController::addRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    char *cmd;
    int tableIndex = findTableNumber(iface);
    int fd;
    struct ifreq ifreqs[20];
    struct ifconf ic;
    unsigned int i;
    if (tableIndex == -1) {
        tableIndex = findTableNumber(""); // look for an empty slot
        if (tableIndex == -1) {
            LOGE("Max number of NATed interfaces reached");
            errno = ENODEV;
            cli->sendMsg(ResponseCode::OperationFailed, "Max number NATed", true);
            return -1;
        }
        strncpy(mInterfaceTable[tableIndex], iface, IFNAMSIZ);
        // Ensure null termination even if truncation happened
        mInterfaceTable[tableIndex][IFNAMSIZ] = 0;
    }

    if ((fd = open("/proc/net/mptcp_pm", O_RDONLY)) != -1) {
	    close(fd);
	    fd = socket(AF_INET, SOCK_DGRAM, 0);
	    if (fd < 0) {
		    LOGE("socket error");
		    return -1;
	    }
            ic.ifc_len = sizeof(ifreqs);
            ic.ifc_buf = (char *) ifreqs;
            if (ioctl(fd, SIOCGIFCONF, &ic) < 0) {
		    LOGE("ioctl error");
		    return -1;
	    }
	    for (i = 0; i < ic.ifc_len / sizeof(struct ifreq); i++) {
		    if (!strcmp(ifreqs[i].ifr_name, iface)) {
			    asprintf(&cmd, "%s rule add from %s table %d", IP_PATH, inet_ntoa(((struct sockaddr_in *)&ifreqs[i].ifr_addr)->sin_addr), tableIndex+BASE_TABLE_NUMBER);
			    system_nosh(cmd);
		    }
	    }
    }

    return modifyRoute(cli, ADD, iface, dest, prefix, gateway, tableIndex);
}

int SecondaryTableController::modifyRoute(SocketClient *cli, char *action, char *iface, char *dest,
        int prefix, char *gateway, int tableIndex) {
    char *cmd;

    if (strcmp("::", gateway) == 0) {
        //  IP tool doesn't like "::" - the equiv of 0.0.0.0 that it accepts for ipv4
        asprintf(&cmd, "%s route %s %s/%d dev %s table %d",
                IP_PATH, action, dest, prefix, iface, tableIndex+BASE_TABLE_NUMBER);
    } else {
        asprintf(&cmd, "%s route %s %s/%d via %s dev %s table %d",
                IP_PATH, action, dest, prefix, gateway, iface, tableIndex+BASE_TABLE_NUMBER);
    }

    if (runAndFree(cli, cmd)) {
        LOGE("ip route %s failed: %s route %s %s/%d via %s dev %s table %d", action,
                IP_PATH, action, dest, prefix, gateway, iface, tableIndex+BASE_TABLE_NUMBER);
        errno = ENODEV;
        cli->sendMsg(ResponseCode::OperationFailed, "ip route modification failed", true);
        return -1;
    }

    if (strcmp(action, ADD) == 0) {
        mInterfaceRuleCount[tableIndex]++;
    } else {
        if (--mInterfaceRuleCount[tableIndex] < 1) {
            mInterfaceRuleCount[tableIndex] = 0;
            mInterfaceTable[tableIndex][0] = 0;
        }
    }
    cli->sendMsg(ResponseCode::CommandOkay, "Route modified", false);
    return 0;
}

int SecondaryTableController::removeRoute(SocketClient *cli, char *iface, char *dest, int prefix,
        char *gateway) {
    char *cmd;
    int tableIndex = findTableNumber(iface);
    int fd;
    struct ifreq ifreqs[20];
    struct ifconf ic;
    unsigned int i;
    if (tableIndex == -1) {
        LOGE("Interface not found");
        errno = ENODEV;
        cli->sendMsg(ResponseCode::OperationFailed, "Interface not found", true);
        return -1;
    }

    if ((fd = open("/proc/net/mptcp_pm", O_RDONLY)) != -1) {
	    close(fd);
	    fd = socket(AF_INET, SOCK_DGRAM, 0);
	    if (fd < 0) {
		    LOGE("socket error");
		    return -1;
	    }
	    ic.ifc_len = sizeof(ifreqs);
	    ic.ifc_buf = (char *) ifreqs;
            if (ioctl(fd, SIOCGIFCONF, &ic) < 0) {
		    LOGE("ioctl error");
		    return -1;
	    }
	    for (i = 0; i < ic.ifc_len / sizeof(struct ifreq); i++) {
		    if (!strcmp(ifreqs[i].ifr_name, iface)) {
			    asprintf(&cmd, "%s rule del from %s table %d", IP_PATH, inet_ntoa(((struct sockaddr_in *)&ifreqs[i].ifr_addr)->sin_addr), tableIndex+BASE_TABLE_NUMBER);
			    system_nosh(cmd);
		    }
	    }
    }

    return modifyRoute(cli, DEL, iface, dest, prefix, gateway, tableIndex);
}

int SecondaryTableController::runAndFree(SocketClient *cli, char *cmd) {
    int ret = 0;
    if (strlen(cmd) >= 255) {
        LOGE("ip command (%s) too long", cmd);
        errno = E2BIG;
        cli->sendMsg(ResponseCode::CommandSyntaxError, "Too long", true);
        free(cmd);
        return -1;
    }
    ret = system_nosh(cmd);
    free(cmd);
    return ret;
}

/* Copyright (C) 2015-2020, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#ifdef WIN32
#include "os_execd/execd.h"
#endif
#include "os_crypto/md5/md5_op.h"
#include "os_net/os_net.h"
#include "wazuh_modules/wmodules.h"
#include "wazuh_modules/wm_sca.h"
#include "syscheck_op.h"
#include "agentd.h"

/* Global variables */
static FILE *fp = NULL;
static char file_sum[34] = "";
static char file[OS_SIZE_1024 + 1] = "";
static const char * IGNORE_LIST[] = { SHAREDCFG_FILENAME, NULL };

// TODO: Remove calls for WIN32

/* Receive events from the server */
int receive_msg()
{
    ssize_t recv_b;
    size_t msg_length;
    int reads = 0;
    static int undefined_msg_logged = 0;
    char buffer[OS_MAXSTR + 1];
    char cleartext[OS_MAXSTR + 1];
    char *tmp_msg;

    memset(cleartext, '\0', OS_MAXSTR + 1);
    memset(buffer, '\0', OS_MAXSTR + 1);

    /* Read until no more messages are available */
    while (1) {
        if (agt->server[agt->rip_id].protocol == IPPROTO_TCP) {
            /* Only one read per call */
            if (reads++) {
                break;
            }

            recv_b = OS_RecvSecureTCP(agt->sock, buffer, OS_MAXSTR);

            // Manager disconnected or error

            if (recv_b <= 0) {
                switch (recv_b) {
                case OS_SOCKTERR:
                    merror("Corrupt payload (exceeding size) received.");
                    break;

                case -1:
                    if (errno == ENOTCONN) {
                        mdebug1("Manager disconnected (ENOTCONN).");
                    } else {
                        merror("Connection socket: %s (%d)", strerror(errno), errno);
                    }
                    break;

                case 0:
                    mdebug1("Manager disconnected.");
                }

                // -1 means that the agent must reconnect
                return -1;
            }
        } else {
            recv_b = recv(agt->sock, buffer, OS_MAXSTR, MSG_DONTWAIT);

            if (recv_b <= 0) {
                break;
            }
        }

        buffer[recv_b] = '\0';

        if (ReadSecMSG(&keys, buffer, cleartext, 0, recv_b - 1, &msg_length, agt->server[agt->rip_id].rip, &tmp_msg) != KS_VALID) {
            mwarn(MSG_ERROR, agt->server[agt->rip_id].rip);
            continue;
        }

        mdebug2("Received message: '%s'", tmp_msg);

        /* Check for commands */
        if (IsValidHeader(tmp_msg)) {
            undefined_msg_logged = 0;

            available_server = (int)time(NULL);
            w_agentd_state_update(UPDATE_ACK, (void *) &available_server);

#ifdef WIN32
            /* Run timeout commands */
            if (agt->execdq >= 0) {
                WinTimeoutRun();
            }
#endif

            /* If it is an active response message */
            if (strncmp(tmp_msg, EXECD_HEADER, strlen(EXECD_HEADER)) == 0) {
                tmp_msg += strlen(EXECD_HEADER);
#ifndef WIN32
                if (agt->execdq >= 0) {
                    if (OS_SendUnix(agt->execdq, tmp_msg, 0) < 0) {
                        merror("Error communicating with execd");
                    }
                }

#else
                /* Run on Windows */
                if (agt->execdq >= 0) {
                    WinExecdRun(tmp_msg);
                }
#endif

                continue;
            }

            /* Force reconnect agent to the manager */
            else if (strncmp(tmp_msg, HC_FORCE_RECONNECT, strlen(HC_FORCE_RECONNECT)) == 0) {
                /* Set lock and wait for it */
                minfo("Wazuh Agent will be reconnected because a reconnect message was received");
                os_setwait();
                w_agentd_state_update(UPDATE_STATUS, (void *) GA_STATUS_NACTIVE);

                /* Send sync message */
                start_agent(0);

                os_delwait();
                w_agentd_state_update(UPDATE_STATUS, (void *) GA_STATUS_ACTIVE);
                continue;
            }

            /* Syscheck */
            else if (strncmp(tmp_msg, HC_SK, strlen(HC_SK)) == 0) {
                ag_send_syscheck(tmp_msg + strlen(HC_SK));
                continue;
            }
            else if (strncmp(tmp_msg, HC_FIM_FILE, strlen(HC_FIM_FILE)) == 0) {
                ag_send_syscheck(tmp_msg + strlen(HC_FIM_FILE));
                continue;
            }
            else if (strncmp(tmp_msg, HC_FIM_REGISTRY, strlen(HC_FIM_REGISTRY)) == 0) {
                ag_send_syscheck(tmp_msg + strlen(HC_FIM_REGISTRY));
                continue;
            }

            /* syscollector */
            else if (strncmp(tmp_msg, HC_SYSCOLLECTOR, strlen(HC_SYSCOLLECTOR)) == 0) {
                wmcom_send(tmp_msg);
                continue;
            }

            /* Ack from server */
            else if (strcmp(tmp_msg, HC_ACK) == 0) {
                continue;
            }

            // Request from manager (or request ack)
            else if (IS_REQ(tmp_msg)) {
                req_push(tmp_msg + strlen(HC_REQUEST), msg_length - strlen(HC_REQUEST) - 3);
                continue;
            }

            /* Security configuration assessment DB request */
            else if (strncmp(tmp_msg,CFGA_DB_DUMP,strlen(CFGA_DB_DUMP)) == 0) {
#ifndef WIN32
                /* Connect to the Security configuration assessment queue */
                if (agt->cfgadq >= 0) {
                    if (OS_SendUnix(agt->cfgadq, tmp_msg, 0) < 0) {
                        mwarn("Error communicating with Security configuration assessment");
                        close(agt->cfgadq);

                        if ((agt->cfgadq = StartMQ(CFGAQUEUE, WRITE, 1)) < 0) {
                            mwarn("Unable to connect to the Security configuration assessment "
                                    "queue (disabled).");
                            agt->cfgadq = -1;
                        } else if (OS_SendUnix(agt->cfgadq, tmp_msg, 0) < 0) {
                            mwarn("Error communicating with Security configuration assessment");
                            close(agt->cfgadq);
                            agt->cfgadq = -1;
                        }
                    }
                } else {
                    if ((agt->cfgadq = StartMQ(CFGAQUEUE, WRITE, 1)) < 0) {
                        mwarn("Unable to connect to the Security configuration assessment "
                            "queue (disabled).");
                        agt->cfgadq = -1;
                    } else {
                         if (OS_SendUnix(agt->cfgadq, tmp_msg, 0) < 0) {
                            mwarn("Error communicating with Security configuration assessment");
                            close(agt->cfgadq);
                            agt->cfgadq = -1;
                        }
                    }
                }
#else
                wm_sca_push_request_win(tmp_msg);
#endif
                continue;
            }

            /* Close any open file pointer if it was being written to */
            if (fp) {
                fclose(fp);
                fp = NULL;
            }

            /* File update message */
            if (strncmp(tmp_msg, FILE_UPDATE_HEADER,
                        strlen(FILE_UPDATE_HEADER)) == 0) {
                char *validate_file;

                tmp_msg += strlen(FILE_UPDATE_HEADER);

                /* Going to after the file sum */
                validate_file = strchr(tmp_msg, ' ');
                if (!validate_file) {
                    continue;
                }

                *validate_file = '\0';

                /* Copy the file sum */
                strncpy(file_sum, tmp_msg, 33);

                /* Set tmp_msg to the beginning of the file name */
                validate_file++;
                tmp_msg = validate_file;

                if ((validate_file = strchr(tmp_msg, '\n')) != NULL) {
                    *validate_file = '\0';
                }

                while ((validate_file = strchr(tmp_msg, '/')) != NULL) {
                    *validate_file = '-';
                }

                if (tmp_msg[0] == '.') {
                    tmp_msg[0] = '-';
                }

                snprintf(file, OS_SIZE_1024, "%s/%s",
                         SHAREDCFG_DIR,
                         tmp_msg);

                fp = fopen(file, "w");
                if (!fp) {
                    merror(FOPEN_ERROR, file, errno, strerror(errno));
                }
            }

            else if (strncmp(tmp_msg, FILE_CLOSE_HEADER,
                             strlen(FILE_CLOSE_HEADER)) == 0) {
                /* No error */
                os_md5 currently_md5;

                /* Close for the rename to work */
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }

                if (file[0] == '\0') {
                    /* Nothing to be done */
                }

                else if (OS_MD5_File(file, currently_md5, OS_TEXT) < 0) {
                    /* Remove file */
                    unlink(file);
                    file[0] = '\0';
                } else {
                    if (strcmp(currently_md5, file_sum) != 0) {
                        mdebug1("Failed md5 for: %s -- deleting.",
                               file);
                        unlink(file);
                    } else {
                        char *final_file;

                        /* Rename the file to its original name */
                        final_file = strrchr(file, '/');
                        if (final_file) {
                            if (strcmp(final_file + 1, SHAREDCFG_FILENAME) == 0) {
                                if (cldir_ex_ignore(SHAREDCFG_DIR, IGNORE_LIST)) {
                                    mwarn("Could not clean up shared directory.");
                                }

                                if(!UnmergeFiles(file, SHAREDCFG_DIR, OS_TEXT)){
                                    char msg_output[OS_MAXSTR];

                                    snprintf(msg_output, OS_MAXSTR, "%c:%s:%s",  LOCALFILE_MQ, "wazuh-agent", AG_IN_UNMERGE);
                                    send_msg(msg_output, -1);
                                }
                                else if (agt->flags.remote_conf && !verifyRemoteConf()) {
                                    if (agt->flags.auto_restart) {
                                        minfo("Agent is restarting due to shared configuration changes.");
                                        restartAgent();
                                    } else {
                                        minfo("Shared agent configuration has been updated.");
                                    }
                                }
                            }
                        } else {
                            /* Remove file */
                            unlink(file);
                        }
                    }

                    file[0] = '\0';
                }
            }

            else {
                mwarn("Unknown message received from server.");
            }
        }

        else if (fp) {
            available_server = (int)time(NULL);
            w_agentd_state_update(UPDATE_ACK, (void *) &available_server);
            fprintf(fp, "%s", tmp_msg);
        }

        else if (!undefined_msg_logged) {
            mwarn("Unknown message received. No action defined.");
            undefined_msg_logged = 1;
        }
    }

    return 0;
}

/* Copyright (C) 2015-2020, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "headers/shared.h"
#include "headers/sec.h"
#include "os_crypto/md5/md5_op.h"
#include "os_crypto/blowfish/bf_op.h"

/* Prototypes */
static void __memclear(char *id, char *name, char *ip, char *key, size_t size) __attribute((nonnull));

static int pass_empty_keyfile = 0;

/* Clear keys entries */
static void __memclear(char *id, char *name, char *ip, char *key, size_t size)
{
    memset(id, '\0', size);
    memset(name, '\0', size);
    memset(key, '\0', size);
    memset(ip, '\0', size);
}

static void move_netdata(keystore *keys, const keystore *old_keys)
{
    unsigned int i;
    int keyid;
    char strsock[16];

    for (i = 0; i < old_keys->keysize; i++) {
        keyid = OS_IsAllowedID(keys, old_keys->keyentries[i]->id);

        if (keyid >= 0 && !strcmp(keys->keyentries[keyid]->ip->ip, old_keys->keyentries[i]->ip->ip)) {
            keys->keyentries[keyid]->rcvd = old_keys->keyentries[i]->rcvd;
            keys->keyentries[keyid]->sock = old_keys->keyentries[i]->sock;
            memcpy(&keys->keyentries[keyid]->peer_info, &old_keys->keyentries[i]->peer_info, sizeof(struct sockaddr_in));

            snprintf(strsock, sizeof(strsock), "%d", keys->keyentries[keyid]->sock);
            rbtree_insert(keys->keytree_sock, strsock, keys->keyentries[keyid]);
        }
    }
}

static void save_removed_key(keystore *keys, const char *key) {
    os_realloc(keys->removed_keys, (keys->removed_keys_size + 1) * sizeof(char*), keys->removed_keys);
    keys->removed_keys[keys->removed_keys_size++] = strdup(key);
}

/* Create the final key */
int OS_AddKey(keystore *keys, const char *id, const char *name, const char *ip, const char *key)
{
    os_md5 filesum1;
    os_md5 filesum2;

    char *tmp_str;
    char _finalstr[KEYSIZE];

    /* Allocate for the whole structure */
    keys->keyentries = (keyentry **)realloc(keys->keyentries,
                                            (keys->keysize + 2) * sizeof(keyentry *));
    if (!keys->keyentries) {
        merror_exit(MEM_ERROR, errno, strerror(errno));
    }

    keys->keyentries[keys->keysize + 1] = keys->keyentries[keys->keysize];
    os_calloc(1, sizeof(keyentry), keys->keyentries[keys->keysize]);

    /* Set configured values for id */
    os_strdup(id, keys->keyentries[keys->keysize]->id);
    rbtree_insert(keys->keytree_id,
               keys->keyentries[keys->keysize]->id,
               keys->keyentries[keys->keysize]);

    /* Agent IP */
    os_calloc(1, sizeof(os_ip), keys->keyentries[keys->keysize]->ip);
    if (OS_IsValidIP(ip, keys->keyentries[keys->keysize]->ip) == 0) {
        merror_exit(INVALID_IP, ip);
    }

    /* We need to remove the "/" from the CIDR */
    if ((tmp_str = strchr(keys->keyentries[keys->keysize]->ip->ip, '/')) != NULL) {
        *tmp_str = '\0';
    }
    rbtree_insert(keys->keytree_ip,
               keys->keyentries[keys->keysize]->ip->ip,
               keys->keyentries[keys->keysize]);

    /* Agent name */
    os_strdup(name, keys->keyentries[keys->keysize]->name);

    /* Initialize the variables */
    keys->keyentries[keys->keysize]->rcvd = 0;
    keys->keyentries[keys->keysize]->local = 0;
    keys->keyentries[keys->keysize]->keyid = keys->keysize;
    keys->keyentries[keys->keysize]->global = 0;
    keys->keyentries[keys->keysize]->fp = NULL;
    keys->keyentries[keys->keysize]->inode = 0;
    keys->keyentries[keys->keysize]->sock = -1;
    keys->keyentries[keys->keysize]->updating_time = 0;
    keys->keyentries[keys->keysize]->rids_node = NULL;
    w_mutex_init(&keys->keyentries[keys->keysize]->mutex, NULL);

    if (keys->flags.rehash_keys) {
        /** Generate final symmetric key **/

        /* MD5 from name, id and key */
        OS_MD5_Str(name, -1, filesum1);
        OS_MD5_Str(id, -1, filesum2);

        /* Generate new filesum1 */
        snprintf(_finalstr, sizeof(_finalstr), "%s%s", filesum1, filesum2);

        /* Use just half of the first MD5 (name/id) */
        OS_MD5_Str(_finalstr, -1, filesum1);
        filesum1[15] = '\0';
        filesum1[16] = '\0';

        /* Second md is just the key */
        OS_MD5_Str(key, -1, filesum2);

        /* Generate final key */
        snprintf(_finalstr, sizeof(_finalstr), "%s%s", filesum2, filesum1);

        /* Final key is 48 * 4 = 192bits */
        os_strdup(_finalstr, keys->keyentries[keys->keysize]->key);

        /* Clean final string from memory */
        memset_secure(_finalstr, '\0', sizeof(_finalstr));
    } else
        os_strdup(key, keys->keyentries[keys->keysize]->key);

    /* Ready for next */
    return keys->keysize++;
}

/* Check if the authentication key file is present */
int OS_CheckKeys()
{
    FILE *fp;

    if (File_DateofChange(KEYS_FILE) < 0) {
        merror(NO_AUTHFILE, KEYS_FILE);
        merror(NO_CLIENT_KEYS);
        return (0);
    }

    fp = fopen(KEYS_FILE, "r");
    if (!fp) {
        /* We can leave from here */
        merror(FOPEN_ERROR, KEYS_FILE, errno, strerror(errno));
        merror(NO_AUTHFILE, KEYS_FILE);
        merror(NO_CLIENT_KEYS);
        return (0);
    }

    fclose(fp);

    /* Authentication keys are present */
    return (1);
}

/* Read the authentication keys */
void OS_ReadKeys(keystore *keys, int rehash_keys, int save_removed)
{
    FILE *fp;

    const char *keys_file = KEYS_FILE;
    char buffer[OS_BUFFER_SIZE + 1];
    char name[KEYSIZE + 1];
    char ip[KEYSIZE + 1];
    char id[KEYSIZE + 1];
    char key[KEYSIZE + 1];
    char *end;
    int id_number;

    /* Check if the keys file is present and we can read it */
    if ((keys->file_change = File_DateofChange(keys_file)) < 0) {
        if (pass_empty_keyfile) {
            mdebug1(NO_AUTHFILE, keys_file);
        } else {
            merror(NO_AUTHFILE, keys_file);
            merror_exit(NO_CLIENT_KEYS);
        }
    }

    keys->inode = File_Inode(keys_file);
    fp = fopen(keys_file, "r");
    if (!fp) {
        if (!pass_empty_keyfile) {
            /* We can leave from here */
            merror(FOPEN_ERROR, keys_file, errno, strerror(errno));
            merror_exit(NO_CLIENT_KEYS);
        }
    }

    /* Initialize hashes */
    keys->keytree_id = rbtree_init();
    keys->keytree_ip = rbtree_init();
    keys->keytree_sock = rbtree_init();

    if (!(keys->keytree_id && keys->keytree_ip && keys->keytree_sock)) {
        merror_exit(MEM_ERROR, errno, strerror(errno));
    }

    /* Initialize structure */
    os_calloc(1, sizeof(keyentry*), keys->keyentries);
    keys->keysize = 0;
    keys->id_counter = 0;
    keys->flags.rehash_keys = rehash_keys;
    keys->flags.save_removed = save_removed;
    w_mutex_init(&keys->keytree_sock_mutex, NULL);

    /* Zero the buffers */
    __memclear(id, name, ip, key, KEYSIZE + 1);
    memset(buffer, '\0', OS_BUFFER_SIZE + 1);

    /* Add additional entry for sender == keysize */
    os_calloc(1, sizeof(keyentry), keys->keyentries[keys->keysize]);
    w_mutex_init(&keys->keyentries[keys->keysize]->mutex, NULL);

    if(fp) {
        /* Read each line. Lines are divided as "id name ip key" */
        while (fgets(buffer, OS_BUFFER_SIZE, fp) != NULL) {
            char *tmp_str;
            char *valid_str;

            if ((buffer[0] == '#') || (buffer[0] == ' ')) {
                continue;
            }

            /* Get ID */
            valid_str = buffer;
            tmp_str = strchr(buffer, ' ');
            if (!tmp_str) {
                merror(INVALID_KEY, buffer);
                continue;
            }

            *tmp_str = '\0';
            tmp_str++;
            strncpy(id, valid_str, KEYSIZE - 1);

            /* Update counter */

            id_number = strtol(id, &end, 10);

            if (!*end && id_number > keys->id_counter)
                keys->id_counter = id_number;

            /* Removed entry */
            if (*tmp_str == '#' || *tmp_str == '!') {
                if (save_removed) {
                    tmp_str[-1] = ' ';
                    tmp_str = strchr(tmp_str, '\n');

                    if (tmp_str) {
                        *tmp_str = '\0';
                    }

                    save_removed_key(keys, buffer);
                }

                continue;
            }

            /* Get name */
            valid_str = tmp_str;
            tmp_str = strchr(tmp_str, ' ');
            if (!tmp_str) {
                merror(INVALID_KEY, buffer);
                continue;
            }

            *tmp_str = '\0';
            tmp_str++;
            strncpy(name, valid_str, KEYSIZE - 1);

            /* Get IP address */
            valid_str = tmp_str;
            tmp_str = strchr(tmp_str, ' ');
            if (!tmp_str) {
                merror(INVALID_KEY, buffer);
                continue;
            }

            *tmp_str = '\0';
            tmp_str++;
            strncpy(ip, valid_str, KEYSIZE - 1);

            /* Get key */
            valid_str = tmp_str;
            tmp_str = strchr(tmp_str, '\n');
            if (tmp_str) {
                *tmp_str = '\0';
            }

            strncpy(key, valid_str, KEYSIZE - 1);

            /* Generate the key hash */
            OS_AddKey(keys, id, name, ip, key);

            /* Clear the memory */
            __memclear(id, name, ip, key, KEYSIZE + 1);

            continue;
        }

        fclose(fp);
        fp = NULL;

        /* Clear one last time before leaving */
        __memclear(id, name, ip, key, KEYSIZE + 1);
    }

    /* Check if there are any agents available */
    if (keys->keysize == 0) {
        if (pass_empty_keyfile) {
            mdebug1(NO_CLIENT_KEYS);
        } else {
            merror_exit(NO_CLIENT_KEYS);
        }
    }

    return;
}

void OS_FreeKey(keyentry *key) {
    if (key->ip) {
        free(key->ip->ip);
        free(key->ip);
    }

    if (key->id) {
        free(key->id);
    }

    if (key->key) {
        free(key->key);
    }

    if (key->name) {
        free(key->name);
    }

    /* Close counter */
    if (key->fp) {
        fclose(key->fp);
    }

    if (key->rids_node) {
        free(key->rids_node);
    }

    w_mutex_destroy(&key->mutex);
    free(key);
}

/* Free the auth keys */
void OS_FreeKeys(keystore *keys)
{
    unsigned int i;

    /* Free the hashes */

    rbtree_destroy(keys->keytree_id);
    rbtree_destroy(keys->keytree_ip);
    rbtree_destroy(keys->keytree_sock);

    for (i = 0; i <= keys->keysize; i++) {
        if (keys->keyentries[i]) {
            OS_FreeKey(keys->keyentries[i]);
            keys->keyentries[i] = NULL;
        }
    }

    /* Zero the entries */
    keys->keysize = 0;
    keys->keytree_id = NULL;
    keys->keytree_ip = NULL;
    keys->keytree_sock = NULL;

    if (keys->removed_keys) {
        for (i = 0; i < keys->removed_keys_size; i++)
            free(keys->removed_keys[i]);

        free(keys->removed_keys);
        keys->removed_keys = NULL;
        keys->removed_keys_size = 0;
    }

    linked_queue_free(keys->opened_fp_queue);

    /* Free structure */
    free(keys->keyentries);
    keys->keyentries = NULL;
    keys->keysize = 0;
    w_mutex_destroy(&keys->keytree_sock_mutex);
}

/* Check if key changed */
int OS_CheckUpdateKeys(const keystore *keys)
{
    return keys->file_change != File_DateofChange(KEYS_FILE) || keys->inode != File_Inode(KEYS_FILE);
}

/* Update the keys if changed */
void OS_UpdateKeys(keystore *keys)
{
    keystore *old_keys;

    mdebug1("Reloading keys");

    mdebug2("OS_DupKeys");
    old_keys = OS_DupKeys(keys);

    mdebug2("Freekeys");
    OS_FreeKeys(keys);

    /* Read keys */
    mdebug2("OS_ReadKeys");
    minfo(ENC_READ);
    OS_ReadKeys(keys, keys->flags.rehash_keys, keys->flags.save_removed);

    mdebug2("OS_StartCounter");
    OS_StartCounter(keys);

    mdebug2("move_netdata");
    move_netdata(keys, old_keys);

    OS_FreeKeys(old_keys);
    free(old_keys);

    mdebug1("Key reloading completed");
}

/* Check if an IP address is allowed to connect */
int OS_IsAllowedIP(keystore *keys, const char *srcip)
{
    keyentry *entry;

    if (srcip == NULL) {
        return (-1);
    }

    entry = (keyentry *) rbtree_get(keys->keytree_ip, srcip);
    if (entry) {
        return ((int)entry->keyid);
    }

    return (-1);
}

/* Check if the agent name is valid */
int OS_IsAllowedName(const keystore *keys, const char *name)
{
    unsigned int i = 0;

    for (i = 0; i < keys->keysize; i++) {
        if (strcmp(keys->keyentries[i]->name, name) == 0) {
            return ((int)i);
        }
    }

    return (-1);
}

int OS_IsAllowedID(keystore *keys, const char *id)
{
    keyentry *entry;

    if (id == NULL) {
        return (-1);
    }

    entry = (keyentry *) rbtree_get(keys->keytree_id, id);
    if (entry) {
        return ((int)entry->keyid);
    }
    return (-1);
}


/* Used for dynamic IP addresses */
int OS_IsAllowedDynamicID(keystore *keys, const char *id, const char *srcip)
{
    keyentry *entry;

    if (id == NULL) {
        return (-1);
    }

    entry = (keyentry *) rbtree_get(keys->keytree_id, id);
    if (entry) {
        if (OS_IPFound(srcip, entry->ip)) {
            return ((int)entry->keyid);
        }
    }

    return (-1);
}

/* Configure to pass if keys file is empty */
void OS_PassEmptyKeyfile() {
    pass_empty_keyfile = 1;
}

/* Delete a key */
int OS_DeleteKey(keystore *keys, const char *id, int purge) {
    int i = OS_IsAllowedID(keys, id);


    if (i < 0)
        return -1;

    /* Save removed key */

    if (keys->flags.save_removed && !purge) {
        char buffer[OS_BUFFER_SIZE + 1];
        keyentry *entry = keys->keyentries[i];
        snprintf(buffer, OS_BUFFER_SIZE, "%s !%s %s %s", entry->id, entry->name, entry->ip->ip, entry->key);
        save_removed_key(keys, buffer);
    }

    rbtree_delete(keys->keytree_id, id);
    rbtree_delete(keys->keytree_ip, keys->keyentries[i]->ip->ip);

    if (keys->keyentries[i]->sock >= 0) {
        char strsock[16] = "";
        snprintf(strsock, sizeof(strsock), "%d", keys->keyentries[i]->sock);
        w_mutex_lock(&keys->keytree_sock_mutex);
        rbtree_delete(keys->keytree_sock, strsock);
        w_mutex_unlock(&keys->keytree_sock_mutex);
    }

    OS_FreeKey(keys->keyentries[i]);
    keys->keysize--;

    if (i < (int)keys->keysize) {
        keys->keyentries[i] = keys->keyentries[keys->keysize];
        keys->keyentries[i]->keyid = i;
    }

    keys->keyentries[keys->keysize] = keys->keyentries[keys->keysize + 1];
    return i;
}

/* Write keystore on client keys file */
int OS_WriteKeys(const keystore *keys) {
    unsigned int i;
    File file;
    char cidr[20];

    if (TempFile(&file, KEYS_FILE, 0) < 0)
        return -1;

    for (i = 0; i < keys->keysize; i++) {
        keyentry *entry = keys->keyentries[i];
        fprintf(file.fp, "%s %s %s %s\n", entry->id, entry->name, OS_CIDRtoStr(entry->ip, cidr, 20) ? entry->ip->ip : cidr, entry->key);
    }

    /* Write saved removed keys */

    for (i = 0; i < keys->removed_keys_size; i++) {
        fprintf(file.fp, "%s\n", keys->removed_keys[i]);
    }

    fclose(file.fp);

    if (OS_MoveFile(file.name, KEYS_FILE) < 0) {
        free(file.name);
        return -1;
    }

    free(file.name);
    return 0;
}

/* Duplicate keystore except key hashes and file pointer */
keystore* OS_DupKeys(const keystore *keys) {
    keystore *copy;
    unsigned int i;

    os_calloc(1, sizeof(keystore), copy);
    os_calloc(keys->keysize + 1, sizeof(keyentry *), copy->keyentries);

    copy->keysize = keys->keysize;
    copy->file_change = keys->file_change;
    copy->inode = keys->inode;
    copy->id_counter = keys->id_counter;
    w_mutex_init(&copy->keytree_sock_mutex, NULL);

    for (i = 0; i <= keys->keysize; i++) {
        copy->keyentries[i] = OS_DupKeyEntry(keys->keyentries[i]);
    }

    if (keys->removed_keys) {
        copy->removed_keys_size = keys->removed_keys_size;
        os_calloc(keys->removed_keys_size, sizeof(char*), copy->removed_keys);

        for (i = 0; i < keys->removed_keys_size; i++)
            copy->removed_keys[i] = strdup(keys->removed_keys[i]);
    }

    return copy;
}

/* Duplicate key entry except key hashes and file pointer */
keyentry * OS_DupKeyEntry(const keyentry * key) {
    keyentry * copy;

    os_calloc(1, sizeof(keyentry), copy);

    copy->rcvd = key->rcvd;
    copy->local = key->local;
    copy->keyid = key->keyid;
    copy->global = key->global;

    if (key->id)
        copy->id = strdup(key->id);

    if (key->key)
        copy->key = strdup(key->key);

    if (key->name)
        copy->name = strdup(key->name);

    if (key->ip) {
        os_calloc(1, sizeof(os_ip), copy->ip);
        copy->ip->ip = strdup(key->ip->ip);
        copy->ip->ip_address = key->ip->ip_address;
        copy->ip->netmask = key->ip->netmask;
    }

    copy->sock = key->sock;
    w_mutex_init(&copy->mutex, NULL);
    copy->peer_info = key->peer_info;

    return copy;
}

// Add socket number into keystore
int OS_AddSocket(keystore * keys, unsigned int i, int sock) {
    char strsock[16] = "";

    snprintf(strsock, sizeof(strsock), "%d", sock);
    keys->keyentries[i]->sock = sock;

    w_mutex_lock(&keys->keytree_sock_mutex);
    int r = rbtree_insert(keys->keytree_sock, strsock, keys->keyentries[i]) ? 2 : rbtree_replace(keys->keytree_sock, strsock, keys->keyentries[i]) ? 1 : 0;
    w_mutex_unlock(&keys->keytree_sock_mutex);

    return r;
}

// Delete socket number from keystore
int OS_DeleteSocket(keystore * keys, int sock) {
    char strsock[16] = "";
    keyentry * entry;
    int retval;

    snprintf(strsock, sizeof(strsock), "%d", sock);
    w_mutex_lock(&keys->keytree_sock_mutex);

    if (entry = rbtree_get(keys->keytree_sock, strsock), entry) {
        if (sock == entry->sock) {
            entry->sock = -1;
        }

        rbtree_delete(keys->keytree_sock, strsock);
    } else {
        retval = -1;
    }

    w_mutex_unlock(&keys->keytree_sock_mutex);
    return retval;
}

int w_get_agent_net_protocol_from_keystore(keystore * keys, const char * agent_id) {

    const int key_id = OS_IsAllowedID(keys, agent_id);

    return (key_id >= 0 ? keys->keyentries[key_id]->net_protocol : key_id);
}

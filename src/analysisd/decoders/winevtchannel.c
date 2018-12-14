/*
* Copyright (C) 2018 Wazuh Inc.
* December 05, 2018.
*
* This program is a free software; you can redistribute it
* and/or modify it under the terms of the GNU General Public
* License (version 2) as published by the FSF - Free Software
* Foundation.
*/

/* Windows eventchannel decoder */

#include "config.h"
#include "eventinfo.h"
#include "alerts/alerts.h"
#include "decoder.h"
#include "external/cJSON/cJSON.h"
#include "plugin_decoders.h"
#include "wazuh_modules/wmodules.h"
#include "os_net/os_net.h"
#include "string_op.h"
#include <time.h>

/* Logging levels */
#define AUDIT		0
#define CRITICAL	1
#define ERROR		2
#define WARNING	    3
#define INFORMATION	4
#define VERBOSE	    5

/* Audit types */
#define AUDIT_FAILURE 0x10000000000000LL
#define AUDIT_SUCCESS 0x20000000000000LL

static OSDecoderInfo *winevt_decoder = NULL;
static int first_time = 0;

void WinevtInit(){

    os_calloc(1, sizeof(OSDecoderInfo), winevt_decoder);
    winevt_decoder->id = getDecoderfromlist(WINEVT_MOD);
    winevt_decoder->name = WINEVT_MOD;
    winevt_decoder->type = OSSEC_RL;
    winevt_decoder->fts = 0;

    mdebug1("WinevtInit completed.");
}

char *replace_string(const char *string, const char *old, const char *new){
    char *ret;
    int i = 0;
    int count = 0;
    int newlen = strlen(new);
    int oldlen = strlen(old);
  
    while (string[i] != '\0') {
        if (strstr(&string[i], old) == &string[i]){
            count++;
            i += oldlen - 1;
        }
        i++;
    }
  
    ret = (char *) malloc(i + count * (newlen - oldlen) + 1);
  
    i = 0;
    while (*string) {
        if (strstr(string, old) == string){
            strcpy(&ret[i], new);
            i += newlen;
            string += oldlen;
        } else {
            ret[i++] = *string++;
        }
    }

    ret[i] = '\0';
    return ret;
}

char *replace_win_format(char *str){
    char *ret1 = NULL;
    char *ret2 = NULL;
    char *ret3 = NULL;

    ret1 = replace_string(str, "\\r", "");
    ret2 = replace_string(ret1, "\\t", "");
    ret3 = replace_string(ret2, "\\n", "");

    os_free(ret1);
    os_free(ret2);

    return ret3;
}

/* Special decoder for Windows eventchannel */
int DecodeWinevt(Eventinfo *lf){
    OS_XML xml;
    cJSON *final_event = cJSON_CreateObject();
    cJSON *json_event = cJSON_CreateObject();
    cJSON *json_system_in = cJSON_CreateObject();
    cJSON *json_eventdata_in = cJSON_CreateObject();
    int level_n;
    unsigned long long int keywords_n;
    XML_NODE node, child;
    int num;
    char *level = NULL;
    char *keywords = NULL;
    char *provider_name = NULL;
    char *msg_from_prov = NULL;
    char *returned_event = NULL;
    char *event = NULL;
    char *find_event = NULL;
    char *end_event = NULL;
    char *real_end = NULL;
    char *find_msg = NULL;
    char *end_msg = NULL;
    char *next = NULL;
    char *category = NULL;
    char aux = 0;
    lf->decoder_info = winevt_decoder;

    os_calloc(OS_MAXSTR, sizeof(char), event);
    os_calloc(OS_MAXSTR, sizeof(char), msg_from_prov);

    find_event = strstr(lf->log, "Event");
    
    if(find_event){
        find_event = find_event + 8;
        end_event = strchr(find_event, '"');
        real_end = end_event;
        if(end_event){
            aux = *(end_event + 1);
            
            if(aux != '}' && aux != ','){
                while(1){
                    next = real_end + 1;
                    real_end = strchr(next,'"');
                    aux = *(real_end + 1);
                    if (aux == '}' || aux == ','){
                        break;
                    }
                }

                end_event = real_end;
            }

            num = end_event - find_event;
            memcpy(event, find_event, num);
            event[num] = '\0';
            find_event = '\0';
            end_event = '\0';
            real_end = '\0';
            next = '\0';
            aux = 0;
        }
    } else {
        mdebug1("Malformed JSON output received. No 'Event' field found");
    }
    char * filtered_string = NULL;
    if(event){
        if (OS_ReadXMLString(event, &xml) < 0){
            first_time++;
            if (first_time > 1){
                mdebug2("Could not read XML string: '%s'", event);
            } else {
                mwarn("Could not read XML string: '%s'", event);
            }
        } else {
            node = OS_GetElementsbyNode(&xml, NULL);
            int i = 0, l = 0;
            if (node && node[i] && (child = OS_GetElementsbyNode(&xml, node[i]))) {
                int j = 0;

                while (child && child[j]){

                    XML_NODE child_attr = NULL;
                    child_attr = OS_GetElementsbyNode(&xml, child[j]);
                    int p = 0;

                    while (child_attr && child_attr[p]){

                        if(child[j]->element && !strcmp(child[j]->element, "System") && child_attr[p]->element){

                            if (!strcmp(child_attr[p]->element, "Provider")) {
                                while(child_attr[p]->attributes[l]){
                                    if (!strcmp(child_attr[p]->attributes[l], "Name")){
                                        os_strdup(child_attr[p]->values[l], provider_name);
                                        cJSON_AddStringToObject(json_system_in, "ProviderName", child_attr[p]->values[l]);
                                    } else if (!strcmp(child_attr[p]->attributes[l], "Guid")){
                                        cJSON_AddStringToObject(json_system_in, "ProviderGuid", child_attr[p]->values[l]);
                                    } else if (!strcmp(child_attr[p]->attributes[l], "EventSourceName")){
                                        cJSON_AddStringToObject(json_system_in, "EventSourceName", child_attr[p]->values[l]);
                                    }
                                    l++;
                                }
                            } else if (!strcmp(child_attr[p]->element, "TimeCreated")) {
                                if(!strcmp(child_attr[p]->attributes[0], "SystemTime")){
                                    cJSON_AddStringToObject(json_system_in, "SystemTime", child_attr[p]->values[0]);
                                }
                            } else if (!strcmp(child_attr[p]->element, "Execution")) {
                                if(!strcmp(child_attr[p]->attributes[0], "ProcessID")){
                                    cJSON_AddStringToObject(json_system_in, "ProcessID", child_attr[p]->values[0]);
                                }
                                if(!strcmp(child_attr[p]->attributes[1], "ThreadID")){
                                    cJSON_AddStringToObject(json_system_in, "ThreadID", child_attr[p]->values[1]);
                                }
                            } else if (!strcmp(child_attr[p]->element, "Channel")) {
                                cJSON_AddStringToObject(json_system_in, "Channel", child_attr[p]->content);
                                if(child_attr[p]->attributes && child_attr[p]->values && !strcmp(child_attr[p]->values[0], "UserID")){
                                    cJSON_AddStringToObject(json_system_in, "UserID", child_attr[p]->values[0]);
                                }
                            } else if (!strcmp(child_attr[p]->element, "Security")) {
                                if(child_attr[p]->attributes && child_attr[p]->values && !strcmp(child_attr[p]->values[0], "UserID")){
                                    cJSON_AddStringToObject(json_system_in, "Security UserID", child_attr[p]->values[0]);
                                }
                            } else if (!strcmp(child_attr[p]->element, "Level")) {
                                os_strdup(child_attr[p]->content, level);
                                cJSON_AddStringToObject(json_system_in, child_attr[p]->element, child_attr[p]->content);
                            } else if (!strcmp(child_attr[p]->element, "Keywords")) {
                                os_strdup(child_attr[p]->content, keywords);
                                cJSON_AddStringToObject(json_system_in, child_attr[p]->element, child_attr[p]->content);
                            } else if (!strcmp(child_attr[p]->element, "Correlation")) {
                            } else {
                                cJSON_AddStringToObject(json_system_in, child_attr[p]->element, child_attr[p]->content);
                            }

                        } else if (child[j]->element && !strcmp(child[j]->element, "EventData") && child_attr[p]->element){
                            if (!strcmp(child_attr[p]->element, "Data") && child_attr[p]->values){
                                for (l = 0; child_attr[p]->attributes[l]; l++) {
                                    if (!strcmp(child_attr[p]->attributes[l], "Name")) {
                                        filtered_string = replace_win_format(child_attr[p]->content);
                                        cJSON_AddStringToObject(json_eventdata_in, child_attr[p]->values[l], filtered_string);
                                        os_free(filtered_string);
                                        break;
                                    } else if(child_attr[p]->content){
                                        filtered_string = replace_win_format(child_attr[p]->content);
                                        mdebug2("Unexpected attribute at EventData (%s).", child_attr[p]->attributes[j]);
                                        cJSON_AddStringToObject(json_eventdata_in, child_attr[p]->values[l], filtered_string);
                                        os_free(filtered_string);
                                    }
                                }
                            } else if (child_attr[p]->content){
                                filtered_string = replace_win_format(child_attr[p]->content);
                                cJSON_AddStringToObject(json_eventdata_in, child_attr[p]->element, filtered_string);
                                os_free(filtered_string);
                            }
                        } else {
                            mdebug1("Unexpected element (%s).", child[j]->element);
                            filtered_string = replace_win_format(child_attr[p]->content);
                            cJSON_AddStringToObject(json_eventdata_in, child_attr[p]->element, filtered_string);
                            os_free(filtered_string);
                        }
                        p++;
                    }

                    OS_ClearNode(child_attr);

                    j++;
                }

                OS_ClearNode(child);
            }

            OS_ClearNode(node);
            OS_ClearXML(&xml);

            if(level && keywords){
                level_n = strtol(level, NULL, 10);
                keywords_n = strtoull(keywords, NULL, 16);

                switch (level_n) {
                    case CRITICAL:
                        category = "CRITICAL";
                        break;
                    case ERROR:
                        category = "ERROR";
                        break;
                    case WARNING:
                        category = "WARNING";
                        break;
                    case INFORMATION:
                        category = "INFORMATION";
                        break;
                    case VERBOSE:
                        category = "VERBOSE";
                        break;
                    case AUDIT:
                        if (keywords_n & AUDIT_FAILURE) {
                            category = "AUDIT_FAILURE";
                            break;
                        } else if (keywords_n & AUDIT_SUCCESS) {
                            category = "AUDIT_SUCCESS";
                            break;
                        }
                        // fall through
                    default:
                        category = "UNKNOWN";
                }

                cJSON_AddStringToObject(json_system_in, "SeverityValue", category);    
            }
        }
    }

    find_msg = strstr(lf->log, "Message");
    if(find_msg){
        find_msg = find_msg + 10;
        end_msg = strchr(find_msg,'\"');
        real_end = end_msg;
        if(end_msg){
            aux = *(end_msg + 1);
            if(aux != '}' && aux != ','){
                while(1){
                    next = real_end + 1;
                    real_end = strchr(next,'"');
                    aux = *(real_end + 1);
                    if (aux == '}' || aux == ','){
                        break;
                    }
                }
                end_msg = real_end;
            }

            num = end_msg - find_msg;
            memcpy(msg_from_prov, find_msg, num);
            msg_from_prov[num] = '\0';
            cJSON_AddStringToObject(json_system_in, "Message", msg_from_prov);
            
            find_msg = '\0';
            end_msg = '\0';
            real_end = '\0';
            next = '\0';
            aux = 0;
        }
    } else {
        mdebug1("Malformed JSON output received. No 'Message' field found");
        cJSON_AddStringToObject(json_system_in, "Message", "No message");
    }

    if(json_system_in){
        cJSON_AddItemToObject(json_event, "System", json_system_in);
    }
    if (json_eventdata_in){
        cJSON_AddItemToObject(json_event, "EventData", json_eventdata_in);
    }

    cJSON_AddItemToObject(final_event, "WinEvtChannel", json_event);

    returned_event = cJSON_PrintUnformatted(final_event);
    
    if (returned_event){
        lf->full_log[strlen(returned_event)] = '\0';
        memcpy(lf->full_log, returned_event, strlen(returned_event) + 1);
    } else {
        lf->full_log = '\0';
    }

    os_free(level);
    os_free(event);
    os_free(filtered_string);
    os_free(keywords);
    os_free(provider_name);
    os_free(msg_from_prov);
    os_free(returned_event);
    OS_ClearXML(&xml);
    cJSON_Delete(final_event);

    return (0);
}
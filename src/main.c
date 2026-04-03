#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

struct response_buf {
    char *data;
    size_t size;
};

static size_t curl_write_response(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct response_buf *buf = (struct response_buf *)userp;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

int main(int argc, char *argv[]) {
    const char *prompt = NULL;
    if (getopt(argc, argv, "p:") == 'p') prompt = optarg;
    if (!prompt) {
        fprintf(stderr, "error: -p flag is required\n");
        return 1;
    }

    const char *api_key = getenv("OPENROUTER_API_KEY");
    const char *base_url = getenv("OPENROUTER_BASE_URL");
    if (!base_url || !*base_url) base_url = "https://openrouter.ai/api/v1";
    if (!api_key || !*api_key) {
        fprintf(stderr, "OPENROUTER_API_KEY is not set\n");
        return 1;
    }

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    cJSON *tools = cJSON_CreateArray();
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *function = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(function, "name", "Read");
    cJSON_AddStringToObject(function, "description", "Read and return the contents of a file");
    cJSON *params = cJSON_AddObjectToObject(function, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *properties = cJSON_AddObjectToObject(params, "properties");
    cJSON *file_path = cJSON_AddObjectToObject(properties, "file_path");
    cJSON_AddStringToObject(file_path, "type", "string");
    cJSON_AddStringToObject(file_path, "description", "The path to the file to read");
    cJSON_AddItemToArray(tools, tool);

    while (1) { 
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", "anthropic/claude-haiku-4.5");
        cJSON_AddItemReferenceToObject(req, "messages", messages);
        cJSON_AddItemReferenceToObject(req, "tools", tools);

        char *body = cJSON_PrintUnformatted(req);
        // cJSON_Delete(req);

        char url[512];
        snprintf(url, sizeof(url), "%s/chat/completions", base_url);

        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

        curl_global_init(CURL_GLOBAL_DEFAULT);
        CURL *curl = curl_easy_init();
        struct response_buf resp = {NULL, 0};
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        free(body);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl error: %s\n", curl_easy_strerror(res));
            free(resp.data);
            return 1;
        }

        cJSON *json = cJSON_Parse(resp.data);
        free(resp.data);
        if (!json) {
            fprintf(stderr, "Failed to parse response JSON\n");
            cJSON_Delete(messages);
            cJSON_Delete(tools);
            return 1;
        }

        cJSON *choices = cJSON_GetObjectItem(json, "choices");
        if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            fprintf(stderr, "no choices in response\n");
            cJSON_Delete(json);
            cJSON_Delete(messages);
            cJSON_Delete(tools);
            return 1;
        }

        cJSON *first = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first, "message");
        // fprintf(stderr, "Logs from your program will appear here!\n")

        cJSON *assistant_msg = cJSON_Duplicate(message, 1);
        cJSON_AddItemToArray(messages, assistant_msg);

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0){
            for (int i = 0; i < cJSON_GetArraySize(tool_calls); i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *tc_func = cJSON_GetObjectItem(tc, "function");
                const char *func_name = cJSON_GetStringValue(cJSON_GetObjectItem(tc_func, "name"));
                const char *args_str = cJSON_GetStringValue(cJSON_GetObjectItem(tc_func, "arguments"));

                if (func_name && strcmp(func_name, "Read") == 0 && args_str) {
                    cJSON *args = cJSON_Parse(args_str);
                    const char *file_path = cJSON_GetStringValue(cJSON_GetObjectItem(args, "file_path"));
                    if (file_path) {
                        FILE *f = fopen(file_path, "rb");
                        if (!f) {
                            fprintf(stderr, "Read: cannot open file: %s\n", file_path);
                            cJSON_Delete(args);
                            cJSON_Delete(json);
                            return 1;
                        }
                        fseek(f, 0, SEEK_END);
                        long fsize = ftell(f);
                        fseek(f, 0, SEEK_SET);
                        char *fbuf = malloc(fsize + 1);
                        fread(fbuf, 1, fsize, f);
                        fclose(f);
                        fbuf[fsize] = '\0';
                        // printf("%s", fbuf);

                        cJSON *msg = cJSON_CreateObject();
                        const char *tc_id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
                        cJSON_AddStringToObject(msg, "role", "tool");
                        cJSON_AddStringToObject(msg, "tool_call_id", tc_id);
                        cJSON_AddStringToObject(msg, "content", fbuf);
                        cJSON_AddItemToArray(messages, msg);
                        free(fbuf);
                        cJSON_Delete(args);
                    }
                }
            }
            cJSON_Delete(json);
            continue;
        } else {
            cJSON *content = cJSON_GetObjectItem(message, "content");
            printf("%s", cJSON_GetStringValue(content));
            cJSON_Delete(json);
            cJSON_Delete(messages);
            cJSON_Delete(tools);
            return 0;
        }
    }
    return 0;
}

#ifndef SERVER_RESPONSE_H
#define SERVER_RESPONSE_H

#include "engine_adapter.h"

int response_write_json(int fd, int status_code,
                        const char *status_text,
                        const char *json_body);
int response_write_engine_result(int fd, const EngineResult *r);
int response_write_error(int fd, int status_code, const char *msg);

#endif

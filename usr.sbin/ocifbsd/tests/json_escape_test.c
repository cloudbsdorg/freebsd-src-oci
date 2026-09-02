/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit test for ocifbsd_json_escape(): escaping, control chars, the injection
 * case (a value trying to close the JSON string and add a key), NULL, and
 * truncation safety on an undersized buffer.
 */
#include <stdio.h>
#include <string.h>
#include "../include/ocifbsd.h"
static int fails;
static void eq(const char*n,const char*got,const char*want){
  int ok=strcmp(got,want)==0; printf("%s: %s -> \"%s\"\n",ok?"PASS":"FAIL",n,got);
  if(!ok){printf("   want \"%s\"\n",want);fails++;}
}
int main(void){
  char b[256];
  eq("plain", ocifbsd_json_escape("hello",b,sizeof b), "hello");
  eq("quote", ocifbsd_json_escape("a\"b",b,sizeof b), "a\\\"b");
  eq("backslash", ocifbsd_json_escape("a\\b",b,sizeof b), "a\\\\b");
  eq("newline", ocifbsd_json_escape("a\nb",b,sizeof b), "a\\nb");
  eq("null", ocifbsd_json_escape(NULL,b,sizeof b), "");
  eq("ctrl", ocifbsd_json_escape("a\x01""b",b,sizeof b), "a\\u0001b");
  /* injection attempt: path that tries to close the string + add a key */
  eq("inject", ocifbsd_json_escape("x\",\"admin\":true",b,sizeof b), "x\\\",\\\"admin\\\":true");
  /* truncation safety: tiny buffer must not overflow and must NUL-terminate */
  char t[4]; ocifbsd_json_escape("aaaaaa",t,sizeof t);
  printf("%s: trunc len=%zu (<=3)\n", strlen(t)<=3?"PASS":"FAIL", strlen(t));
  if(strlen(t)>3)fails++;
  printf("%s (%d)\n", fails?"FAILED":"ALL PASSED", fails);
  return fails?1:0;
}

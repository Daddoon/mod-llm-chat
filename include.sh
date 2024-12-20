#!/usr/bin/env bash

CURL_LIBS="$(pkg-config --libs libcurl)"
CURL_CFLAGS="$(pkg-config --cflags libcurl)"

AC_ADD_LINK_FLAGS="$AC_ADD_LINK_FLAGS $CURL_LIBS"
AC_ADD_COMPILE_FLAGS="$AC_ADD_COMPILE_FLAGS $CURL_CFLAGS" 
# Copyright (c) PHYTEC Messtechnik GmbH
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ipc_rproc_rpmsg)

target_sources(app PRIVATE src/main.c)

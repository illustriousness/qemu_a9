/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020/12/31     Bernard      Add license info
 */

#include <stdint.h>
#include <stdio.h>
#include <rtthread.h>
#include "shell.h"
int main(void)
{
    rt_kprintf("Hello RT-Thread!\n");
    finsh_set_prompt("\033[31m" "msh" "\033[0m");
    return 0;
}

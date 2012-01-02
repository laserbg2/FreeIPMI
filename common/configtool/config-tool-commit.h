/*
 * Copyright (C) 2003-2012 FreeIPMI Core Team
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#ifndef _CONFIG_TOOL_COMMIT_H_
#define _CONFIG_TOOL_COMMIT_H_

#include "config-tool-common.h"
#include "pstdout.h"

config_err_t config_commit_section (pstdout_state_t pstate,
                                    struct config_section *section,
                                    struct config_arguments *cmd_args,
                                    void *arg);

config_err_t config_commit (pstdout_state_t pstate,
                            struct config_section *sections,
                            struct config_arguments *cmd_args,
                            void *arg);

#endif /* _CONFIG_TOOL_COMMIT_H_ */
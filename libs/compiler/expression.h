/*
 *	Copyright 2021 Andrey Terekhov, Ilya Andreev
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include "operations.h"
#include "syntax.h"
#include "tree.h"


#ifdef __cplusplus
extern "C" {
#endif

/**
 *	Get expression type
 *
 *	@param	expr	Expression
 *
 *	@return	Expression type
 */
item_t expression_get_type(const node *const expr);

/**
 *	Check if expression is lvalue
 *
 *	@param	expr	Expression for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
bool expression_is_lvalue(const node *const expr);

/**
 *	Get expression location
 *
 *	@param	expr	Expression
 *
 *	@return	Expression location
 */
location expression_get_location(const node *const expr);

#ifdef __cplusplus
extern "C" }
#endif

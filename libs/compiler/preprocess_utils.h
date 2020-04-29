/*
 *	Copyright 2019 Andrey Terekhov, Victor Y. Fadeev
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
#include "preprocess_global.h"
#include "context.h"

int equal_reprtab(int i, int j, preprocess_context *context);
int macro_keywords(preprocess_context *context, compiler_context *c_context);
int collect_mident(preprocess_context *context, compiler_context *c_context);
void space_end_line(preprocess_context *context, compiler_context *c_context);
void space_skip(preprocess_context *context, compiler_context *c_context);
void space_skip_str(preprocess_context *context, compiler_context *c_context);
int is_letter(preprocess_context *context);
int is_digit(preprocess_context *context);

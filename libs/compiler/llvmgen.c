/*
 *	Copyright 2021 Andrey Terekhov, Ivan S. Arkhipov
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

#include "llvmgen.h"
#include <string.h>
#include "AST.h"
#include "errors.h"
#include "hash.h"
#include "uniprinter.h"


#define MAX_FUNCTION_ARGS 128
#define MAX_PRINTF_ARGS 128


static const size_t HASH_TABLE_SIZE = 1024;
static const size_t IS_STATIC = 0;
static const size_t MAX_DIMENSIONS = SIZE_MAX - 2;		// Из-за OP_SLICE


typedef enum ANSWER
{
	AREG,								/**< Ответ находится в регистре */
	ACONST,								/**< Ответ является константой */
	ALOGIC,								/**< Ответ является логическим значением */
	AMEM,								/**< Ответ находится в памяти */
	ASTR,								/**< Ответ является строкой */
	ANULL,								/**< Ответ является null */
} answer_t;

typedef enum LOCATION
{
	LREG,								/**< Переменная находится в регистре */
	LMEM,								/**< Переменная находится в памяти */
	LFREE,								/**< Свободный запрос значения */
} location_t;

typedef struct information
{
	syntax *sx;								/**< Структура syntax с таблицами */

	size_t register_num;					/**< Номер регистра */
	item_t label_num;						/**< Номер метки */
	item_t init_num;						/**< Счётчик для инициализации */
	item_t block_num;						/**< Номер блока */

	size_t request_reg;						/**< Регистр на запрос */
	location_t variable_location;			/**< Расположение переменной */

	size_t answer_reg;						/**< Регистр с ответом */
	item_t answer_const;					/**< Константа с ответом */
	size_t answer_string;					/**< Индекс строки с ответом */
	double answer_const_double;				/**< Константа с ответом типа double */
	answer_t answer_kind;					/**< Вид ответа */

	item_t label_true;						/**< Метка перехода при true */
	item_t label_false;						/**< Метка перехода при false */
	item_t label_break;						/**< Метка перехода для break */
	item_t label_continue;					/**< Метка перехода для continue */
	item_t label_ternary_end;				/**< Метка перехода в конец тернарного выражения */

	hash arrays;							/**< Хеш таблица с информацией о массивах:
												@с key		 - смещение массива
												@c value[0]	 - флаг статичности
												@c value[1..MAX] - границы массива */

	bool was_stack_functions;				/**< Истина, если использовались стековые функции */
	bool was_dynamic;						/**< Истина, если в функции были динамические массивы */
	bool was_file;							/**< Истина, если была работа с файлами */
	bool was_abs;							/**< Истина, если был вызов abs */
	bool was_fabs;							/**< Истина, если был вызов fabs */
	bool was_function[BEGIN_USER_FUNC];		/**< Массив флагов библиотечных функций из builtin_t */
	bool is_main;							/**< Истина, если обрабатывается main */
} information;


static void emit_statement(information *const info, const node *const nd);
static void emit_compound_statement(information *const info, const node *const nd, const bool is_function_body);
static void emit_expression(information *const info, const node *const nd);
static void emit_declaration(information *const info, const node *const nd, const bool is_local);


// TODO: такая функция есть в builder, хотелось бы не дублировать
static inline item_t usual_arithmetic_conversions(information *const info
	, const item_t left_type, const item_t right_type)
{
	return type_is_integer(info->sx, left_type) && type_is_integer(info->sx, right_type)
		? type_get_class(info->sx, left_type) == TYPE_CHARACTER && type_get_class(info->sx, left_type) == TYPE_CHARACTER
		? TYPE_CHARACTER : TYPE_INTEGER : TYPE_FLOATING;
}

static item_t array_get_type(information *const info, const item_t array_type)
{
	item_t type = array_type;
	while (type_is_array(info->sx, type))
	{
		type = type_array_get_element_type(info->sx, type);
	}

	return type;
}

static size_t array_get_dim(information *const info, const item_t array_type)
{
	size_t i = 0;
	item_t type = array_type;
	while (type_is_array(info->sx, type))
	{
		type = type_array_get_element_type(info->sx, type);
		i++;
	}

	return i;
}

static void type_to_io(information *const info, const item_t type)
{
	const type_t type_class = type_get_class(info->sx, type);
	switch (type_class)
	{
		case TYPE_VARARG:
			uni_printf(info->sx->io, "...");
			break;

		case TYPE_CHARACTER:
			uni_printf(info->sx->io, "i8");
			break;

		case TYPE_INTEGER:
			uni_printf(info->sx->io, "i32");
			break;

		case TYPE_FLOATING:
			uni_printf(info->sx->io, "double");
			break;

		case TYPE_VOID:
			uni_printf(info->sx->io, "void");
			break;

		case TYPE_STRUCTURE:
			uni_printf(info->sx->io, "%%struct_opt.%" PRIitem, type);
			break;

		case TYPE_POINTER:
		{
			type_to_io(info, type_pointer_get_element_type(info->sx, type));
			uni_printf(info->sx->io, "*");
		}
		break;

		case TYPE_ARRAY:
		{
			type_to_io(info, type_array_get_element_type(info->sx, type));
			uni_printf(info->sx->io, "*");
		}
		break;

		case TYPE_FILE:
		{
			uni_printf(info->sx->io, "%%struct._IO_FILE");
			info->was_file = true;
		}
		break;

		case TYPE_FUNCTION:
		{
			type_to_io(info, type_function_get_return_type(info->sx, type));
			uni_printf(info->sx->io, " (");

			const size_t parameter_amount = type_function_get_parameter_amount(info->sx, type);
			for (size_t i = 0; i < parameter_amount; i++)
			{
				type_to_io(info, type_function_get_parameter_type(info->sx, type, i));

				if (i != parameter_amount - 1)
				{
					uni_printf(info->sx->io, ", ");
				}
			}
			uni_printf(info->sx->io, ")");
		}
		break;

		default:
			break;
	}
}

static void operation_to_io(information *const info, const binary_t operation_type, const item_t type)
{
	switch (operation_type)
	{
		case BIN_ADD_ASSIGN:
		case BIN_ADD:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "add nsw" : "fadd");
			break;

		case BIN_SUB_ASSIGN:
		case BIN_SUB:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "sub nsw" : "fsub");
			break;

		case BIN_MUL_ASSIGN:
		case BIN_MUL:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "mul nsw" : "fmul");
			break;

		case BIN_DIV_ASSIGN:
		case BIN_DIV:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "sdiv" : "fdiv");
			break;

		case BIN_REM_ASSIGN:
		case BIN_REM:
			uni_printf(info->sx->io, "srem");
			break;

		case BIN_SHL_ASSIGN:
		case BIN_SHL:
			uni_printf(info->sx->io, "shl");
			break;

		case BIN_SHR_ASSIGN:
		case BIN_SHR:
			uni_printf(info->sx->io, "ashr");
			break;

		case BIN_AND_ASSIGN:
		case BIN_AND:
			uni_printf(info->sx->io, "and");
			break;

		case BIN_XOR_ASSIGN:
		case BIN_XOR:
			uni_printf(info->sx->io, "xor");
			break;

		case BIN_OR_ASSIGN:
		case BIN_OR:
			uni_printf(info->sx->io, "or");
			break;

		case BIN_EQ:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp eq" : "fcmp oeq");
			break;
		case BIN_NE:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp ne" : "fcmp one");
			break;
		case BIN_LT:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp slt" : "fcmp olt");
			break;
		case BIN_GT:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp sgt" : "fcmp ogt");
			break;
		case BIN_LE:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp sle" : "fcmp ole");
			break;
		case BIN_GE:
			uni_printf(info->sx->io, type_is_integer(info->sx, type) ? "icmp sge" : "fcmp oge");
			break;
		default:
			break;
	}
}

static void to_code_operation_reg_reg(information *const info, const binary_t operation
	, const size_t fst, const size_t snd, const item_t type)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, type);
	uni_printf(info->sx->io, " ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " %%.%zu, %%.%zu\n", fst, snd);
}

static void to_code_operation_reg_const_integer(information *const info, const binary_t operation
	, const size_t fst, const item_t snd, const item_t type)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_INTEGER);
	uni_printf(info->sx->io, " ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " %%.%zu, %" PRIitem "\n", fst, snd);
}

static void to_code_operation_reg_const_double(information *const info, const binary_t operation
	, const size_t fst, const double snd)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_FLOATING);
	uni_printf(info->sx->io, " double %%.%zu, %f\n", fst, snd);
}

static void to_code_operation_const_reg_integer(information *const info, const binary_t operation
	, const item_t fst, const size_t snd, const item_t type)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_INTEGER);
	uni_printf(info->sx->io, " ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " %" PRIitem ", %%.%zu\n", fst, snd);
}

static void to_code_operation_const_reg_double(information *const info, const binary_t operation
	, const double fst, const size_t snd)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_FLOATING);
	uni_printf(info->sx->io, " double %f, %%.%zu\n", fst, snd);
}

static void to_code_operation_reg_null(information *const info, const binary_t operation
	, const size_t fst, const item_t type)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_INTEGER);
	uni_printf(info->sx->io, " ");
	type_to_io(info, type);
	uni_printf(info->sx->io, "* %%.%zu, null\n", fst);
}

static void to_code_operation_null_reg(information *const info, const binary_t operation
	, const size_t snd, const item_t type)
{
	uni_printf(info->sx->io, " %%.%zu = ", info->register_num);
	operation_to_io(info, operation, TYPE_INTEGER);
	uni_printf(info->sx->io, " ");
	type_to_io(info, type);
	uni_printf(info->sx->io, "* null, %%.%zu\n", snd);
}

static void to_code_load(information *const info, const size_t result, const size_t id, const item_t type
	, const bool is_array, const bool is_local)
{
	uni_printf(info->sx->io, " %%.%zu = load ", result);
	type_to_io(info, type);
	uni_printf(info->sx->io, ", ");
	type_to_io(info, type);
	uni_printf(info->sx->io, "* %s%s.%zu, align 4\n", is_local ? "%" : "@", is_array ? "" : "var", id);
}

static void to_code_store_reg(information *const info, const size_t reg, const size_t id, const item_t type
	, const bool is_array, const bool is_pointer, const bool is_local)
{
	uni_printf(info->sx->io, " store ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " %%%s.%zu, ", is_pointer ? "var" : "", reg);
	type_to_io(info, type);
	uni_printf(info->sx->io, "* %s%s.%zu, align 4\n", is_local ? "%" : "@", is_array ? "" : "var", id);
}

static inline void to_code_store_const_integer(information *const info, const item_t arg, const size_t id
	, const bool is_array, const bool is_local, const item_t type)
{
	uni_printf(info->sx->io, " store ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " %" PRIitem ", ", arg);
	type_to_io(info, type);
	uni_printf(info->sx->io, "* %s%s.%zu, align 4\n", is_local ? "%" : "@", is_array ? "" : "var", id);
}

static inline void to_code_store_const_double(information *const info, const double arg, const size_t id
	, const bool is_array, const bool is_local)
{
	uni_printf(info->sx->io, " store double %f, double* %s%s.%zu, align 4\n"
		, arg, is_local ? "%" : "@", is_array ? "" : "var", id);
}

static void to_code_store_null(information *const info, const size_t id, const item_t type)
{
	uni_printf(info->sx->io, " store ");
	type_to_io(info, type);
	uni_printf(info->sx->io, " null, ");
	type_to_io(info, type);
	uni_printf(info->sx->io, "* %%var.%zu, align 4\n", id);
}

static void to_code_try_zext_to(information *const info)
{
	if (info->answer_kind != ALOGIC)
	{
		return;
	}

	uni_printf(info->sx->io, " %%.%zu = zext i1 %%.%zu to i32\n", info->register_num, info->answer_reg);
	info->answer_kind = AREG;
	info->answer_reg = info->register_num++;
}

static inline void to_code_label(information *const info, const item_t label_num)
{
	uni_printf(info->sx->io, " label%" PRIitem ":\n", label_num);
}

static inline void to_code_unconditional_branch(information *const info, const item_t label_num)
{
	uni_printf(info->sx->io, " br label %%label%" PRIitem "\n", label_num);
}

static inline void to_code_conditional_branch(information *const info)
{
	uni_printf(info->sx->io, " br i1 %%.%zu, label %%label%" PRIitem ", label %%label%" PRIitem "\n"
		, info->answer_reg, info->label_true, info->label_false);
}

static void to_code_stack_save(information *const info, const item_t index)
{
	// команды сохранения состояния стека
	uni_printf(info->sx->io, " %%dyn.%" PRIitem " = alloca i8*, align 4\n", index);
	uni_printf(info->sx->io, " %%.%zu = call i8* @llvm.stacksave()\n", info->register_num);
	uni_printf(info->sx->io, " store i8* %%.%zu, i8** %%dyn.%" PRIitem ", align 4\n"
		, info->register_num, index);
	info->register_num++;

	info->was_stack_functions = true;
}

static void to_code_stack_load(information *const info, const item_t index)
{
	// команды восстановления состояния стека
	uni_printf(info->sx->io, " %%.%zu = load i8*, i8** %%dyn.%" PRIitem ", align 4\n"
		, info->register_num, index);
	uni_printf(info->sx->io, " call void @llvm.stackrestore(i8* %%.%zu)\n", info->register_num);
	info->register_num++;

	info->was_stack_functions = true;
}

static void to_code_alloc_array_static(information *const info, const size_t index, const item_t type, const bool is_local)
{
	if (is_local)
	{
		uni_printf(info->sx->io, " %%arr.%" PRIitem " = alloca ", hash_get_key(&info->arrays, index));
	}
	else
	{
		uni_printf(info->sx->io, "@arr.%" PRIitem " = common global ", hash_get_key(&info->arrays, index));
	}

	const size_t dim = hash_get_amount_by_index(&info->arrays, index) - 1;
	if (dim == 0 || dim > MAX_DIMENSIONS)
	{
		system_error(such_array_is_not_supported);
		return;
	}

	for (size_t i = 1; i <= dim; i++)
	{
		uni_printf(info->sx->io, "[%" PRIitem " x ", hash_get_by_index(&info->arrays, index, i));
	}
	type_to_io(info, type);

	for (size_t i = 1; i <= dim; i++)
	{
		uni_printf(info->sx->io, "]");
	}
	uni_printf(info->sx->io, "%s, align 4\n", is_local ? "" : " zeroinitializer");
}

static void to_code_alloc_array_dynamic(information *const info, const size_t index, const item_t type)
{
	// выделение памяти на стеке
	item_t to_alloc = hash_get_by_index(&info->arrays, index, 1);

	const size_t dim = hash_get_amount_by_index(&info->arrays, index) - 1;
	if (dim == 0 || dim > MAX_DIMENSIONS)
	{
		system_error(such_array_is_not_supported);
		return;
	}

	for (size_t i = 2; i <= dim; i++)
	{
		uni_printf(info->sx->io, " %%.%zu = mul nuw i32 %%.%" PRIitem ", %%.%" PRIitem "\n"
			, info->register_num, to_alloc, hash_get_by_index(&info->arrays, index, i));
		to_alloc = info->register_num++;
	}
	uni_printf(info->sx->io, " %%dynarr.%" PRIitem " = alloca ", hash_get_key(&info->arrays, index));
	type_to_io(info, type);
	uni_printf(info->sx->io, ", i32 %%.%" PRIitem ", align 4\n", to_alloc);
}

static void to_code_slice(information *const info, const item_t id, const size_t cur_dimension
	, const item_t prev_slice, const item_t type, const bool is_local)
{
	uni_printf(info->sx->io, " %%.%zu = getelementptr inbounds ", info->register_num);
	const size_t dimensions = hash_get_amount(&info->arrays, id) - 1;

	if (hash_get(&info->arrays, id, IS_STATIC))
	{
		for (size_t i = dimensions - cur_dimension; i <= dimensions; i++)
		{
			uni_printf(info->sx->io, "[%" PRIitem " x ", hash_get(&info->arrays, id, i));
		}
		type_to_io(info, type);

		for (size_t i = dimensions - cur_dimension; i <= dimensions; i++)
		{
			uni_printf(info->sx->io, "]");
		}
		uni_printf(info->sx->io, ", ");

		for (size_t i = dimensions - cur_dimension; i <= dimensions; i++)
		{
			uni_printf(info->sx->io, "[%" PRIitem " x ", hash_get(&info->arrays, id, i));
		}
		type_to_io(info, type);

		for (size_t i = dimensions - cur_dimension; i <= dimensions; i++)
		{
			uni_printf(info->sx->io, "]");
		}

		if (cur_dimension == dimensions - 1)
		{
			uni_printf(info->sx->io, "* %sarr.%" PRIitem ", i32 0", is_local ? "%" : "@", id);
		}
		else
		{
			uni_printf(info->sx->io, "* %%.%" PRIitem ", i32 0", prev_slice);
		}
	}
	else if (cur_dimension == dimensions - 1)
	{
		type_to_io(info, type);
		uni_printf(info->sx->io, ", ");
		type_to_io(info, type);
		uni_printf(info->sx->io, "* %%dynarr.%" PRIitem, id);
	}
	else
	{
		type_to_io(info, type);
		uni_printf(info->sx->io, ", ");
		type_to_io(info, type);
		uni_printf(info->sx->io, "* %%.%" PRIitem, prev_slice);
	}

	if (info->answer_kind == AREG)
	{
		uni_printf(info->sx->io, ", i32 %%.%zu\n", info->answer_reg);
	}
	else // if (info->answer_kind == ACONST)
	{
		uni_printf(info->sx->io, ", i32 %" PRIitem "\n", info->answer_const);
	}

	info->register_num++;
}


static void check_type_and_branch(information *const info)
{
	switch (info->answer_kind)
	{
		case ACONST:
			to_code_unconditional_branch(info, info->answer_const ? info->label_true : info->label_false);
			break;
		case AREG:
		{
			to_code_operation_reg_const_integer(info, BIN_NE, info->answer_reg, 0, TYPE_INTEGER);
			info->answer_reg = info->register_num++;
		}
		case ALOGIC:
			to_code_conditional_branch(info);
			break;
		default:
			break;
	}
}


/*
 *	 ______     __  __     ______   ______     ______     ______     ______     __     ______     __   __     ______
 *	/\  ___\   /\_\_\_\   /\  == \ /\  == \   /\  ___\   /\  ___\   /\  ___\   /\ \   /\  __ \   /\ "-.\ \   /\  ___\
 *	\ \  __\   \/_/\_\/_  \ \  _-/ \ \  __<   \ \  __\   \ \___  \  \ \___  \  \ \ \  \ \ \/\ \  \ \ \-.  \  \ \___  \
 *	 \ \_____\   /\_\/\_\  \ \_\    \ \_\ \_\  \ \_____\  \/\_____\  \/\_____\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/_____/   \/_/\/_/   \/_/     \/_/ /_/   \/_____/   \/_____/   \/_____/   \/_/   \/_____/   \/_/ \/_/   \/_____/
 */


/**
 *	Emit cast expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_cast_expression(information *const info, const node *const nd)
{
	const item_t target_type = expression_get_type(nd);
	const item_t source_type = expression_cast_get_source_type(nd);

	const node expression_to_cast = expression_cast_get_operand(nd);
	emit_expression(info, &expression_to_cast);

	uni_printf(info->sx->io, " %%.%zu = sitofp ", info->register_num);
	type_to_io(info, source_type);
	uni_printf(info->sx->io, " %%.%zu to ", info->answer_reg);
	type_to_io(info, target_type);
	uni_printf(info->sx->io, "\n");

	info->answer_reg = info->register_num++;
}

/**
 *	Emit identifier expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_identifier_expression(information *const info, const node *const nd)
{
	item_t type = expression_get_type(nd);
	const size_t id = expression_identifier_get_id(nd);
	const bool is_local = ident_is_local(info->sx, id);
	const bool is_addr_to_val = info->variable_location == LMEM;

	if (is_addr_to_val)
	{
		to_code_load(info, info->register_num, id, type, false, is_local);
		info->register_num++;
		info->variable_location = LREG;
		type = type_pointer_get_element_type(info->sx, type);
	}

	if (type_is_array(info->sx, type))
	{
		info->answer_const = 0;
		to_code_slice(info, id, 0, 0, array_get_type(info, type), is_local);
		info->answer_reg = info->register_num;
	}
	else
	{
		to_code_load(info, info->register_num, is_addr_to_val ? info->register_num - 1 : id, type
			, is_addr_to_val, is_addr_to_val || is_local);
		info->answer_reg = info->register_num++;
	}
	info->answer_kind = AREG;
}

/**
 *	Emit literal expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_literal_expression(information *const info, const node *const nd)
{
	const item_t type = expression_get_type(nd);

	if (type_is_string(info->sx, type))
	{
		info->answer_string = expression_literal_get_string(nd);
		info->answer_kind = ASTR;
	}
	else if (type_is_integer(info->sx, type))
	{
		const item_t num = expression_literal_get_integer(nd);
		if (info->variable_location == LMEM)
		{
			to_code_store_const_integer(info, num, info->request_reg, false
				, ident_is_local(info->sx, info->request_reg), type);
			info->answer_kind = AREG;
		}
		else
		{
			info->answer_kind = ACONST;
			info->answer_const = num;
		}
	}
	else if (type_is_floating(type))
	{
		const double num = expression_literal_get_floating(nd);
		if (info->variable_location == LMEM)
		{
			to_code_store_const_double(info, num, info->request_reg, false
				, ident_is_local(info->sx, info->request_reg));
			info->answer_kind = AREG;
		}
		else
		{
			info->answer_kind = ACONST;
			info->answer_const_double = num;
		}
	}
	else // nullptr
	{
		info->answer_kind = ANULL;
	}
}

/**
 *	Emit initialization of lvalue
 *
 *	@param	info			Encoder
 *	@param	nd				Node in AST
 *	@param	id				Identifier of target lvalue
 *	@param	cur_dimension	Current dimension of slice
 */
static void emit_one_dimension_subscript(information *const info, const node *const nd, const size_t id
	, const size_t cur_dimension)
{
	// TODO: научиться обрабатывать многомерные динамические массивы
	const node base = expression_subscript_get_base(nd);
	const size_t dimensions = hash_get_amount(&info->arrays, id) - 1;
	const bool is_local = ident_is_local(info->sx, id);
	const item_t arr_type = ident_get_type(info->sx, id);
	const item_t type = array_get_type(info, arr_type);

	if (cur_dimension != dimensions - 1)
	{
		emit_one_dimension_subscript(info, &base, id, cur_dimension + 1);
	}

	info->variable_location = LFREE;
	const node index = expression_subscript_get_index(nd);
	emit_expression(info, &index);
	to_code_slice(info, id, cur_dimension, info->register_num - 1, type, is_local);
}

/**
 *	Emit initialization of lvalue
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_subscript_expression(information *const info, const node *const nd)
{
	node base = expression_subscript_get_base(nd);
	while (expression_get_class(&base) == EXPR_SUBSCRIPT)
	{
		base = expression_subscript_get_base(&base);
	}

	const size_t id = expression_identifier_get_id(&base);
	const location_t location = info->variable_location;

	emit_one_dimension_subscript(info, nd, id, 0);

	if (location != LMEM)
	{
		const item_t arr_type = ident_get_type(info->sx, id);
		const item_t type = array_get_type(info, arr_type);

		to_code_load(info, info->register_num, info->register_num - 1, type, true, true);
		info->register_num++;
	}

	info->answer_reg = info->register_num - 1;
	info->answer_kind = AREG;
}

/**
 *	Emit call expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_call_expression(information *const info, const node *const nd)
{
	item_t arguments[MAX_FUNCTION_ARGS];
	double arguments_double[MAX_FUNCTION_ARGS];
	answer_t arguments_type[MAX_FUNCTION_ARGS];
	item_t arguments_value_type[MAX_FUNCTION_ARGS];

	const item_t func_type = expression_get_type(nd);

	const node callee = expression_call_get_callee(nd);
	const size_t args = expression_call_get_arguments_amount(nd);
	if (args > MAX_FUNCTION_ARGS)
	{
		system_error(too_many_arguments);
		return;
	}

	// FIXME: а если это не функция, а указатель на функцию?
	const size_t func_ref = expression_identifier_get_id(&callee);
	if (func_ref < BEGIN_USER_FUNC)
	{
		info->was_function[func_ref] = true;
	}

	for (size_t i = 0; i < args; i++)
	{
		info->variable_location = LFREE;
		const node argument = expression_call_get_argument(nd, i);
		arguments_value_type[i] = expression_get_type(&argument);
		emit_expression(info, &argument);
		to_code_try_zext_to(info);
		// TODO: сделать параметры других типов (логическое)
		arguments_type[i] = info->answer_kind;

		if (info->answer_kind == AREG)
		{
			arguments[i] = info->answer_reg;
		}
		else if (info->answer_kind == ASTR)
		{
			arguments[i] = (item_t)info->answer_string;
		}
		else if (type_is_integer(info->sx, arguments_value_type[i])) // ACONST
		{
			arguments[i] = info->answer_const;
		}
		else // double
		{
			arguments_double[i] = info->answer_const_double;
		}
	}

	if (!type_is_void(func_type))
	{
		uni_printf(info->sx->io, " %%.%zu =", info->register_num);
		info->answer_kind = AREG;
		info->answer_reg = info->register_num++;
	}
	uni_printf(info->sx->io, " call ");
	type_to_io(info, expression_get_type(&callee));
	uni_printf(info->sx->io, " @%s(", ident_get_spelling(info->sx, func_ref));

	for (size_t i = 0; i < args; i++)
	{
		if (i != 0)
		{
			uni_printf(info->sx->io, ", ");
		}

		if (arguments_type[i] == ASTR)
		{
			const size_t index = (size_t)arguments[i];
			const size_t string_length = strings_length(info->sx, index);

			uni_printf(info->sx->io, "i8* getelementptr inbounds "
				"([%zu x i8], [%zu x i8]* @.str%zu, i32 0, i32 0)"
				, string_length + 1
				, string_length + 1
				, index);

			continue;
		}

		type_to_io(info, arguments_value_type[i]);
		if (arguments_type[i] == AREG)
		{
			uni_printf(info->sx->io, " %%.%" PRIitem, arguments[i]);
		}
		else if (arguments_type[i] == ASTR)
		{
			const size_t index = (size_t)arguments[i];
			const size_t string_length = strings_length(info->sx, index);

			uni_printf(info->sx->io, "i8* getelementptr inbounds "
				"([%zu x i8], [%zu x i8]* @.str%zu, i32 0, i32 0)"
				, string_length + 1
				, string_length + 1
				, index);
		}
		else if (type_is_integer(info->sx, arguments_value_type[i])) // ACONST
		{
			uni_printf(info->sx->io, " %" PRIitem, arguments[i]);
		}
		else // double
		{
			uni_printf(info->sx->io, " %f", arguments_double[i]);
		}
	}
	uni_printf(info->sx->io, ")\n");
}

/**
 *	Emit member expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_member_expression(information *const info, const node *const nd)
{
	const item_t place = expression_member_get_member_index(nd);
	const item_t elem_type = expression_get_type(nd);

	const node base = expression_member_get_base(nd);

	item_t type = expression_get_type(&base);
	const size_t id = expression_identifier_get_id(&base);

	const bool is_pointer = type_is_pointer(info->sx, type);
	if (type_is_pointer(info->sx, type))
	{
		to_code_load(info, info->register_num++, id, type, false, ident_is_local(info->sx, id));
		type = type_pointer_get_element_type(info->sx, type);
	}

	uni_printf(info->sx->io, " %%.%zu = getelementptr inbounds %%struct_opt.%" PRIitem ", " 
		"%%struct_opt.%" PRIitem "* %%%s.%zu, i32 0, i32 %" PRIitem "\n", info->register_num, type, type
		, is_pointer ? "" : "var", is_pointer ? info->register_num - 1 : id, place);

	if (info->variable_location != LMEM)
	{
		info->register_num++;
		to_code_load(info, info->register_num, info->register_num - 1, elem_type, true, true);
		info->answer_kind = AREG;
	}

	info->answer_reg = info->register_num++;
}

/**
 *	Emit increment/decrement expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_inc_dec_expression(information *const info, const node *const nd)
{
	const unary_t operation = expression_unary_get_operator(nd);
	const item_t operation_type = expression_get_type(nd);

	// TODO: вообще тут может быть и поле структуры
	const node operand = expression_unary_get_operand(nd);
	bool is_array_or_pointer = expression_get_class(&operand) == EXPR_SUBSCRIPT || expression_get_class(&operand) == EXPR_UNARY;
	size_t id = 0;
	if (!is_array_or_pointer)
	{
		id = expression_identifier_get_id(&operand);
	}
	else // OP_SLICE_IDENT
	{
		info->variable_location = LMEM;
		emit_expression(info, &operand); // OP_SLICE_IDENT
		id = (size_t)info->answer_reg;
	}

	to_code_load(info, info->register_num, id, operation_type, is_array_or_pointer, ident_is_local(info->sx, id));
	info->answer_kind = AREG;
	info->answer_reg = info->register_num++;

	switch (operation)
	{
		case UN_PREINC:
		case UN_PREDEC:
			info->answer_reg = info->register_num;
		case UN_POSTINC:
		case UN_POSTDEC:
		{
			if (type_is_integer(info->sx, operation_type))
			{
				to_code_operation_reg_const_integer(info, operation == UN_PREINC || operation == UN_POSTINC ? BIN_ADD : BIN_SUB
					, info->register_num - 1, 1, operation_type);
			}
			else // double
			{
				to_code_operation_reg_const_double(info, operation == UN_PREINC || operation == UN_POSTINC ? BIN_ADD : BIN_SUB
					, info->register_num - 1, 1.0);
			}
		}
		break;
		default:
			break;
	}

	to_code_store_reg(info, info->register_num, id, operation_type, is_array_or_pointer, false, ident_is_local(info->sx, id));
	info->register_num++;
}

/**
 *	Emit unary expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_unary_expression(information *const info, const node *const nd)
{
	const unary_t operator = expression_unary_get_operator(nd);
	const node operand = expression_unary_get_operand(nd);

	switch (operator)
	{
		case UN_POSTINC:
		case UN_POSTDEC:
		case UN_PREINC:
		case UN_PREDEC:
			emit_inc_dec_expression(info, nd);
			return;

		case UN_MINUS:
		case UN_NOT:
		{
			const item_t operation_type = expression_get_type(nd);

			info->variable_location = LREG;
			emit_expression(info, &operand);

			to_code_try_zext_to(info);

			if (operator == UN_MINUS && type_is_integer(info->sx, operation_type))
			{
				to_code_operation_const_reg_integer(info, BIN_SUB, 0, info->answer_reg, operation_type);
			}
			else if (operator == UN_NOT)
			{
				to_code_operation_reg_const_integer(info, BIN_XOR, info->answer_reg, -1, operation_type);
			}
			else if (operator == UN_MINUS && type_is_floating(operation_type))
			{
				to_code_operation_const_reg_double(info, BIN_SUB, 0, info->answer_reg);
			}

			info->answer_kind = AREG;
			info->answer_reg = info->register_num++;
			return;
		}

		case UN_LOGNOT:
		{
			const item_t temp = info->label_true;
			info->label_true =  info->label_false;
			info->label_false = temp;

			emit_expression(info, &operand);
			return;
		}

		case UN_ADDRESS:
		{
			// TODO: тут тоже не только идентификатор может быть
			info->answer_reg = expression_identifier_get_id(&operand);
			info->answer_kind = AMEM;
			return;
		}

		case UN_INDIRECTION:
		{
			// FIXME: а если это нескалярный тип? (например, структура)
			info->variable_location = info->variable_location == LMEM ? LREG : LMEM;
			emit_expression(info, &operand);
			return;
		}
		case UN_ABS:
		{
			const item_t type = expression_get_type(nd);
			info->variable_location = LFREE;
			emit_expression(info, &operand);

			uni_printf(info->sx->io, " %%.%zu = call ", info->register_num);
			type_to_io(info, type);

			if (type_is_integer(info->sx, type))
			{
				uni_printf(info->sx->io, " @abs(");
				info->was_abs = true;
			}
			else
			{
				uni_printf(info->sx->io, " @llvm.fabs.f64(");
				info->was_fabs = true;
			}

			type_to_io(info, type);
			uni_printf(info->sx->io, " %%.%zu)\n", info->answer_reg);

			info->answer_kind = AREG;
			info->answer_reg = info->register_num++;
		}
		break;

		default:
			// TODO: оставшиеся унарные операторы
			return;
	}
}

/**
 *	Emit non-assignment binary expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_integral_expression(information *const info, const node *const nd, const answer_t kind)
{
	const binary_t operation = expression_binary_get_operator(nd);
	item_t operation_type = expression_get_type(nd);

	info->variable_location = LFREE;
	const node LHS = expression_binary_get_LHS(nd);
	const item_t answer_type = expression_get_type(&LHS);
	emit_expression(info, &LHS);

	if (kind == ALOGIC)
	{
		operation_type = usual_arithmetic_conversions(info, answer_type, expression_get_type(nd));
	}

	to_code_try_zext_to(info);

	// TODO: спрятать эти переменные в одну структуру и возвращать ее из emit_expr
	const answer_t left_kind = info->answer_kind;
	const size_t left_reg = info->answer_reg;
	const item_t left_const = info->answer_const;
	const double left_const_double = info->answer_const_double;

	info->variable_location = LFREE;
	const node RHS = expression_binary_get_RHS(nd);
	emit_expression(info, &RHS);

	to_code_try_zext_to(info);

	const answer_t right_kind = info->answer_kind;
	const size_t right_reg = info->answer_reg;
	const item_t right_const = info->answer_const;
	const double right_const_double = info->answer_const_double;

	if (left_kind == AREG && right_kind == AREG)
	{
		to_code_operation_reg_reg(info, operation, left_reg, right_reg, operation_type);
	}
	else if (left_kind == AREG && right_kind == ACONST && type_is_integer(info->sx, operation_type))
	{
		to_code_operation_reg_const_integer(info, operation, left_reg, right_const, operation_type);
	}
	else if (left_kind == AREG && right_kind == ACONST) // double
	{
		to_code_operation_reg_const_double(info, operation, left_reg, right_const_double);
	}
	else if (left_kind == ACONST && right_kind == AREG && type_is_integer(info->sx, operation_type))
	{
		to_code_operation_const_reg_integer(info, operation, left_const, right_reg, operation_type);
	}
	else if (left_kind == ACONST && right_kind == AREG) // double
	{
		to_code_operation_const_reg_double(info, operation, left_const_double, right_reg);
	}
	else if (left_kind == AREG && right_kind == ANULL)
	{
		to_code_operation_reg_null(info, operation, left_reg, operation_type);
	}
	else if (left_kind == ANULL && right_kind == AREG)
	{
		to_code_operation_null_reg(info, operation, right_reg, operation_type);
	}

	info->answer_reg = info->register_num++;
	info->answer_kind = kind;
}

/**
 *	Emit assignment expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_assignment_expression(information *const info, const node *const nd)
{
	const binary_t assignment_type = expression_binary_get_operator(nd);
	const item_t operation_type = expression_get_type(nd);

	// TODO: вообще тут может быть и вырезка из структуры
	const node LHS = expression_binary_get_LHS(nd);
	size_t id = 0;
	bool is_array = expression_get_class(&LHS) == EXPR_SUBSCRIPT;
	if (!is_array)
	{
		id = expression_identifier_get_id(&LHS);
	}
	else // OP_SLICE_IDENT
	{
		info->variable_location = LMEM;
		emit_expression(info, &LHS); // OP_SLICE_IDENT or UN_ADDRESS
		id = info->answer_reg;
	}

	info->variable_location = LFREE;
	const node RHS = expression_binary_get_RHS(nd);
	emit_expression(info, &RHS);

	to_code_try_zext_to(info);
	size_t result = info->answer_reg;

	if (assignment_type != BIN_ASSIGN)
	{
		to_code_load(info, info->register_num, id, operation_type, is_array, ident_is_local(info->sx, id));
		info->register_num++;

		if (info->answer_kind == AREG)
		{
			to_code_operation_reg_reg(info, assignment_type, info->register_num - 1, info->answer_reg, operation_type);
		}
		else if (type_is_integer(info->sx, operation_type)) // ACONST и операция =
		{
			to_code_operation_reg_const_integer(info, assignment_type, info->register_num - 1
				, info->answer_const, operation_type);
		}
		else if (type_is_floating(operation_type))
		{
			to_code_operation_reg_const_double(info, assignment_type, info->register_num - 1
				, info->answer_const_double);
		}

		result = info->register_num++;
		info->answer_kind = AREG;
	}

	if (info->answer_kind == AREG || info->answer_kind == AMEM)
	{
		to_code_store_reg(info, result, id, operation_type, is_array
			, info->answer_kind == AMEM, ident_is_local(info->sx, id));

		info->answer_kind = AREG;
		info->answer_reg = result;
	}
	else if (type_is_integer(info->sx, operation_type)) // ACONST и операция =
	{
		to_code_store_const_integer(info, info->answer_const, id, is_array, ident_is_local(info->sx, id), operation_type);
	}
	else if (type_is_floating(operation_type))
	{
		to_code_store_const_double(info, info->answer_const_double, id, is_array, ident_is_local(info->sx, id));
	}
	else
	{
		to_code_store_null(info, id, operation_type);

		info->answer_kind = ANULL;
	}
}

/**
 *	Emit binary expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_binary_expression(information *const info, const node *const nd)
{
	const binary_t operator = expression_binary_get_operator(nd);
	if (operation_is_assignment(operator))
	{
		emit_assignment_expression(info, nd);
		return;
	}

	switch (operator)
	{
		case BIN_MUL:
		case BIN_DIV:
		case BIN_REM:
		case BIN_ADD:
		case BIN_SUB:
		case BIN_SHL:
		case BIN_SHR:
		case BIN_AND:
		case BIN_XOR:
		case BIN_OR:
			emit_integral_expression(info, nd, AREG);
			return;

		case BIN_LT:
		case BIN_GT:
		case BIN_LE:
		case BIN_GE:
		case BIN_EQ:
		case BIN_NE:
			emit_integral_expression(info, nd, ALOGIC);
			return;

		// TODO: протестировать и при необходимости реализовать случай, когда && и || есть в арифметических выражениях
		case BIN_LOG_OR:
		case BIN_LOG_AND:
		{
			const item_t label_next = info->label_num++;
			const item_t old_label_true = info->label_true;
			const item_t old_label_false = info->label_false;

			if (operator == BIN_LOG_OR)
			{
				info->label_false = label_next;
			}
			else // (operator == OP_LOG_AND)
			{
				info->label_true = label_next;
			}

			const node LHS = expression_binary_get_LHS(nd);
			emit_expression(info, &LHS);

			// TODO: сделать обработку других ответов
			// постараться использовать функцию check_type_and_branch
			if (info->answer_kind == ALOGIC)
			{
				to_code_conditional_branch(info);
			}

			to_code_label(info, label_next);
			info->label_true = old_label_true;
			info->label_false = old_label_false;

			const node RHS = expression_binary_get_RHS(nd);
			emit_expression(info, &RHS);
			return;
		}

		default:
			// TODO: оставшиеся бинарные операторы
			return;
	}
}

/**
 *	Emit ternary expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_ternary_expression(information *const info, const node *const nd)
{
	const item_t old_label_true = info->label_true;
	const item_t old_label_false = info->label_false;
	item_t label_then = info->label_num++;
	item_t label_else = info->label_num++;
	const item_t label_end = info->label_num++;

	info->label_true = label_then;
	info->label_false = label_else;

	info->variable_location = LFREE;
	const node condition = expression_ternary_get_condition(nd);
	emit_expression(info, &condition);

	check_type_and_branch(info);

	to_code_label(info, label_then);

	info->variable_location = LFREE;
	const node LHS = expression_ternary_get_LHS(nd);
	const bool then_is_ternary = expression_get_class(&LHS) == EXPR_TERNARY;
	emit_expression(info, &LHS);

	const answer_t then_answer = info->answer_kind;
	const item_t then_reg = info->answer_reg;
	const item_t then_const = info->answer_const;

	if (then_is_ternary)
	{
		label_then = info->label_ternary_end;
	}

	to_code_unconditional_branch(info, label_end);
	to_code_label(info, label_else);

	info->variable_location = LFREE;
	const node RHS = expression_ternary_get_RHS(nd);
	const bool else_is_ternary = expression_get_class(&RHS) == EXPR_TERNARY;
	emit_expression(info, &RHS);

	const answer_t else_answer = info->answer_kind;
	const item_t else_reg = info->answer_reg;
	const item_t else_const = info->answer_const;

	if (else_is_ternary)
	{
		label_else = info->label_ternary_end;
	}

	to_code_unconditional_branch(info, label_end);
	to_code_label(info, label_end);

	uni_printf(info->sx->io, " %%.%zu = phi ", info->register_num);
	type_to_io(info, expression_get_type(nd));
	uni_printf(info->sx->io, " [ %s%" PRIitem ", %%label%" PRIitem " ]", then_answer == AREG ? "%." : ""
		, then_answer == AREG ? then_reg : then_const, label_then);
	uni_printf(info->sx->io, ", [ %s%" PRIitem ", %%label%" PRIitem " ]\n", else_answer == AREG ? "%." : ""
		, else_answer == AREG ? else_reg : else_const, label_else);

	info->answer_kind = AREG;
	info->answer_reg = info->register_num++;

	info->label_true = old_label_true;
	info->label_false = old_label_false;
	info->label_ternary_end = label_end;
}

/**
 *	Emit expression
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_expression(information *const info, const node *const nd)
{
	switch (expression_get_class(nd))
	{
		case EXPR_CAST:
			emit_cast_expression(info, nd);
			return;

		case EXPR_IDENTIFIER:
			emit_identifier_expression(info, nd);
			return;

		case EXPR_LITERAL:
			emit_literal_expression(info, nd);
			return;

		case EXPR_SUBSCRIPT:
			emit_subscript_expression(info, nd);
			return;

		case EXPR_CALL:
			emit_call_expression(info, nd);
			return;

		case EXPR_MEMBER:
			emit_member_expression(info, nd);
			return;

		case EXPR_UNARY:
			emit_unary_expression(info, nd);
			return;

		case EXPR_BINARY:
			emit_binary_expression(info, nd);
			return;

		case EXPR_TERNARY:
			emit_ternary_expression(info, nd);
			return;

		default:
			// TODO: генерация оставшихся выражений
			return;
	}
}

/**
 *	Emit initialization of lvalue
 *
 *	@param	info			Encoder
 *	@param	nd				Node in AST
 *	@param	id				Identifier of target lvalue
 *	@param	arr_type		Array type of target lvalue
 *	@param	cur_dimension	Current dimension of slice
 *	@param	prev_slice		Register of previous slice, if it exists
 *	@param	is_local		Is array local or global
 */
static void emit_one_dimension_initialization(information *const info, const node *const nd, const item_t id
	, const item_t arr_type, const size_t cur_dimension, const item_t prev_slice, const bool is_local)
{
	const size_t size = expression_initializer_get_size(nd);
	const item_t type = array_get_type(info, arr_type);

	// TODO: тут пока инициализация константами, нужно реализовать более общий случай
	for (size_t i = 0; i < size; i++)
	{
		info->answer_const = (item_t)i;
		info->answer_kind = ACONST;
		const size_t slice_reg = (size_t)info->register_num;
		if (is_local)
		{
			to_code_slice(info, id, cur_dimension, prev_slice, type, true);
		}

		info->variable_location = LFREE;
		const node initializer = expression_initializer_get_subexpr(nd, i);

		// последнее измерение
		if (cur_dimension == 0)
		{
			emit_expression(info, &initializer);

			if (info->answer_kind == AREG)
			{
				to_code_store_reg(info, info->answer_reg, slice_reg, type, true, false, is_local);
			}
			// константа типа int
			else if (type_is_integer(info->sx, type))
			{
				if (is_local)
				{
					to_code_store_const_integer(info, info->answer_const, slice_reg, true, true, type);
				}
				else
				{
					uni_printf(info->sx->io, "i32 %" PRIitem "%s", info->answer_const, i != size - 1 ? ", " : "], align 4\n");
				}
			}
			// константа типа double
			else
			{
				if (is_local)
				{
					to_code_store_const_double(info, info->answer_const_double, slice_reg, true, true);
				}
				else
				{
					uni_printf(info->sx->io, "double %f%s", info->answer_const_double, i != size - 1 ? ", " : "], align 4\n");
				}
			}
		}
		else
		{
			emit_one_dimension_initialization(info, &initializer, id, arr_type, cur_dimension - 1
				, (item_t)slice_reg, true);
		}
	}
}

/**
 *	Emit initialization of lvalue
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 *	@param	id			Identifier of target lvalue
 *	@param	arr_type	Array type of target lvalue
 */
static void emit_initialization(information *const info, const node *const nd, const item_t id, const item_t arr_type)
{
	if (expression_get_class(nd) == EXPR_INITIALIZER && type_is_array(info->sx, expression_get_type(nd)))
	{
		const size_t dimensions = array_get_dim(info, arr_type);
		const size_t N = expression_initializer_get_size(nd);

		const size_t index = hash_get_index(&info->arrays, id);

		node list_expression = *nd;
		for (size_t i = 0; i < dimensions; i++)
		{
			hash_set_by_index(&info->arrays, index, 1 + i, (item_t)expression_initializer_get_size(&list_expression));
			list_expression = expression_initializer_get_subexpr(&list_expression, 0);
		}

		const item_t type = array_get_type(info, arr_type);
		const bool is_local = ident_is_local(info->sx, (size_t)id);

		// TODO: с глобальными массивами хорошо бы как-то покрасивее сделать
		// а неконстантными выражениями глобальный массив может инициализироваться?
		if (is_local)
		{
			to_code_alloc_array_static(info, index, type, true);
		}
		else
		{
			uni_printf(info->sx->io, "@arr.%" PRIitem " = global [%zu x ", id, N);
			type_to_io(info, type);
			uni_printf(info->sx->io, "] [");
		}

		emit_one_dimension_initialization(info, nd, id, arr_type, dimensions - 1, 0, is_local);
	}
	// TODO: надо реализовать для большего количества измерений. Там сложность в том, что последнее измерение может иметь различные границы
	// массив инициализируется строкой
	else if (expression_get_class(nd) == EXPR_LITERAL && type_is_array(info->sx, expression_get_type(nd)))
	{
		const char *string = string_get(info->sx, expression_literal_get_string(nd));
		const size_t length = strings_length(info->sx, expression_literal_get_string(nd));

		const size_t index = hash_get_index(&info->arrays, id);
		hash_set_by_index(&info->arrays, index, 1, (item_t)length);

		const item_t type = array_get_type(info, arr_type);
		to_code_alloc_array_static(info, index, type, true);

		for (size_t i = 0; i < length; i++)
		{
			info->answer_const = (item_t)i;
			info->answer_kind = ACONST;
			const size_t slice_reg = (size_t)info->register_num;
			to_code_slice(info, id, 0, 0, type, true);
			to_code_store_const_integer(info, string[i], slice_reg, true, true, type);
		}
	}
}


/*
 *	 _____     ______     ______     __         ______     ______     ______     ______   __     ______     __   __     ______
 *	/\  __-.  /\  ___\   /\  ___\   /\ \       /\  __ \   /\  == \   /\  __ \   /\__  _\ /\ \   /\  __ \   /\ "-.\ \   /\  ___\
 *	\ \ \/\ \ \ \  __\   \ \ \____  \ \ \____  \ \  __ \  \ \  __<   \ \  __ \  \/_/\ \/ \ \ \  \ \ \/\ \  \ \ \-.  \  \ \___  \
 *	 \ \____-  \ \_____\  \ \_____\  \ \_____\  \ \_\ \_\  \ \_\ \_\  \ \_\ \_\    \ \_\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/____/   \/_____/   \/_____/   \/_____/   \/_/\/_/   \/_/ /_/   \/_/\/_/     \/_/   \/_/   \/_____/   \/_/ \/_/   \/_____/
 */


/**
 *	Emit variable declaration
 *
 *	@param	info	Encoder
 *	@param	nd		Node in AST
 */
static void emit_variable_declaration(information *const info, const node *const nd, const bool is_local)
{
	// TODO: объявления глобальных переменных
	const size_t id = declaration_variable_get_id(nd);
	const bool has_init = declaration_variable_has_initializer(nd);
	const item_t type = ident_get_type(info->sx, id);

	if (!type_is_array(info->sx, type) && is_local) // обычная переменная int a; или struct point p;
	{
		uni_printf(info->sx->io, " %%var.%zu = alloca ", id);
		type_to_io(info, type);
		uni_printf(info->sx->io, ", align 4\n");

		if (declaration_variable_has_initializer(nd))
		{
			info->variable_location = LFREE;
			info->request_reg = id;

			const node initializer = declaration_variable_get_initializer(nd);
			emit_expression(info, &initializer);

			if (info->answer_kind == ACONST)
			{
				if (type_is_integer(info->sx, type))
				{
					to_code_store_const_integer(info, info->answer_const, info->request_reg, false, is_local, type);
				}
				else
				{
					to_code_store_const_double(info, info->answer_const_double, info->request_reg, false, is_local);
				}
			}
			else if (info->answer_kind == AREG)
			{
				to_code_store_reg(info, info->answer_reg, id, type, false, false, is_local);
			}

		}
	}
	else if (!type_is_array(info->sx, type) && !is_local) // глобальные переменные
	{
		uni_printf(info->sx->io, "@var.%zu = ", id);

		if (declaration_variable_has_initializer(nd))
		{
			info->variable_location = LFREE;

			const node initializer = declaration_variable_get_initializer(nd);
			emit_expression(info, &initializer);

			if (info->answer_kind == ACONST)
			{
				uni_printf(info->sx->io, "global ");
				type_to_io(info, type);
				if (type_is_integer(info->sx, type))
				{
					uni_printf(info->sx->io, " %" PRIitem ", align 4\n", info->answer_const);
				}
				else
				{
					uni_printf(info->sx->io, " %f, align 4\n", info->answer_const_double);
				}
			}
		}
		else
		{
			uni_printf(info->sx->io, "common global ");
			type_to_io(info, type);
			uni_printf(info->sx->io, " %s, align 4\n", type_is_integer(info->sx, type) ? "0" : "0.0");
		}
	}
	else // массив
	{
		const size_t dimensions = array_get_dim(info, type);
		const item_t element_type = array_get_type(info, type);
		const size_t index = hash_add(&info->arrays, id, 1 + dimensions);
		hash_set_by_index(&info->arrays, index, IS_STATIC, 1);

		// получение и сохранение границ
		const size_t bounds = declaration_variable_get_dim_amount(nd);
		for (size_t j = 1; j <= bounds; j++)
		{
			info->variable_location = LFREE;
			const node dim_size = declaration_variable_get_dim_expr(nd, j - 1);
			emit_expression(info, &dim_size);

			if (!has_init)
			{
				if (info->answer_kind == ACONST)
				{
					if (!hash_get_by_index(&info->arrays, index, IS_STATIC))
					{
						system_error(array_borders_cannot_be_static_dynamic);
					}

					hash_set_by_index(&info->arrays, index, j, info->answer_const);
				}
				else // if (info->answer_kind == AREG) динамический массив
				{
					if (hash_get_by_index(&info->arrays, index, IS_STATIC) && j > 1)
					{
						system_error(array_borders_cannot_be_static_dynamic);
					}

					hash_set_by_index(&info->arrays, index, j, info->answer_reg);
					hash_set_by_index(&info->arrays, index, IS_STATIC, 0);
				}
			}
		}

		if (hash_get_by_index(&info->arrays, index, IS_STATIC) && !has_init)
		{
			to_code_alloc_array_static(info, index, element_type, is_local);
		}
		else if (!has_init) // объявление массива, если он динамический
		{
			if (!info->was_dynamic)
			{
				to_code_stack_save(info, -1);
			}

			to_code_alloc_array_dynamic(info, index, element_type);
			info->was_dynamic = true;
		}
	}

	if (has_init)
	{
		const node initializer = declaration_variable_get_initializer(nd);
		emit_initialization(info, &initializer, id, type);
	}
}

/**
 * Emit function definition
 *
 * @param	info	Encoder
 * @param	nd		Node in AST
 */
static void emit_function_definition(information *const info, const node *const nd)
{
	const size_t ref_ident = declaration_function_get_id(nd);
	const item_t func_type = ident_get_type(info->sx, ref_ident);
	const item_t ret_type = ref_ident != info->sx->ref_main ? type_function_get_return_type(info->sx, func_type) : TYPE_INTEGER;
	const size_t parameters = type_function_get_parameter_amount(info->sx, func_type);
	info->was_dynamic = false;

	uni_printf(info->sx->io, "define ");
	type_to_io(info, ret_type);
	
	if (ref_ident == info->sx->ref_main)
	{
		uni_printf(info->sx->io, " @main(");
		info->is_main = true;
	}
	else
	{
		uni_printf(info->sx->io, " @%s(", ident_get_spelling(info->sx, ref_ident));
	}

	for (size_t i = 0; i < parameters; i++)
	{
		uni_printf(info->sx->io, i == 0 ? "" : ", ");

		const item_t param_type = type_function_get_parameter_type(info->sx, func_type, i);
		type_to_io(info, param_type);
	}
	uni_printf(info->sx->io, ") {\n");

	for (size_t i = 0; i < parameters; i++)
	{
		const size_t id = declaration_function_get_param(nd, i);
		const item_t param_type = ident_get_type(info->sx, id);

		uni_printf(info->sx->io, " %%var.%zu = alloca ", id);
		type_to_io(info, param_type);
		uni_printf(info->sx->io, ", align 4\n");

		uni_printf(info->sx->io, " store ");
		type_to_io(info, param_type);
		uni_printf(info->sx->io, " %%%zu, ", i);
		type_to_io(info, param_type);
		uni_printf(info->sx->io, "* %%var.%zu, align 4\n", id);
	}

	const node body = declaration_function_get_body(nd);
	emit_compound_statement(info, &body, true);

	if (type_is_void(ret_type))
	{
		if (info->was_dynamic)
		{
			to_code_stack_load(info, -1);
		}
		uni_printf(info->sx->io, " ret void\n");
	}
	else if (ref_ident == info->sx->ref_main)
	{
		uni_printf(info->sx->io, " ret i32 0\n");
		info->is_main = false;
	}
	uni_printf(info->sx->io, "}\n\n");
}

static void emit_declaration(information *const info, const node *const nd, const bool is_local)
{
	switch (declaration_get_class(nd))
	{
		case DECL_VAR:
			emit_variable_declaration(info, nd, is_local);
			return;

		case DECL_FUNC:
			emit_function_definition(info, nd);
			return;

		default:
			// С объявлением типа ничего делать не нужно
			return;
	}
}


/*
 *	 ______     ______   ______     ______   ______     __    __     ______     __   __     ______   ______
 *	/\  ___\   /\__  _\ /\  __ \   /\__  _\ /\  ___\   /\ "-./  \   /\  ___\   /\ "-.\ \   /\__  _\ /\  ___\
 *	\ \___  \  \/_/\ \/ \ \  __ \  \/_/\ \/ \ \  __\   \ \ \-./\ \  \ \  __\   \ \ \-.  \  \/_/\ \/ \ \___  \
 *	 \/\_____\    \ \_\  \ \_\ \_\    \ \_\  \ \_____\  \ \_\ \ \_\  \ \_____\  \ \_\\"\_\    \ \_\  \/\_____\
 *	  \/_____/     \/_/   \/_/\/_/     \/_/   \/_____/   \/_/  \/_/   \/_____/   \/_/ \/_/     \/_/   \/_____/
 */


/**
 *	Emit labeled statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_labeled_statement(information *const info, const node *const nd)
{
	const item_t label = -(item_t)statement_labeled_get_label(nd);
	to_code_unconditional_branch(info, label);
	to_code_label(info, label);

	const node substmt = statement_labeled_get_substmt(nd);
	emit_statement(info, &substmt);
}

/**
 *	Emit compound statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_compound_statement(information *const info, const node *const nd, const bool is_function_body)
{
	const item_t block_num = info->block_num++;
	if (!is_function_body)
	{
		to_code_stack_save(info, block_num);
	}

	const size_t size = statement_compound_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node substmt = statement_compound_get_substmt(nd, i);
		emit_statement(info, &substmt);
	}

	if (!is_function_body)
	{
		to_code_stack_load(info, block_num);
	}
}

/**
 *	Emit if statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_if_statement(information *const info, const node *const nd)
{
	const item_t old_label_true = info->label_true;
	const item_t old_label_false = info->label_false;
	const item_t label_if = info->label_num++;
	const item_t label_else = info->label_num++;
	const item_t label_end = info->label_num++;

	info->label_true = label_if;
	info->label_false = label_else;

	info->variable_location = LFREE;
	const node condition = statement_if_get_condition(nd);
	emit_expression(info, &condition);

	check_type_and_branch(info);

	to_code_label(info, label_if);

	const node then_substmt = statement_if_get_then_substmt(nd);
	emit_statement(info, &then_substmt);

	to_code_unconditional_branch(info, label_end);
	to_code_label(info, label_else);

	if (statement_if_has_else_substmt(nd))
	{
		const node else_substmt = statement_if_get_else_substmt(nd);
		emit_statement(info, &else_substmt);
	}

	to_code_unconditional_branch(info, label_end);
	to_code_label(info, label_end);

	info->label_true = old_label_true;
	info->label_false = old_label_false;
}

/**
 *	Emit while statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_while_statement(information *const info, const node *const nd)
{
	const item_t old_label_true = info->label_true;
	const item_t old_label_false = info->label_false;
	const item_t old_label_break = info->label_break;
	const item_t old_label_continue = info->label_continue;
	const item_t label_condition = info->label_num++;
	const item_t label_body = info->label_num++;
	const item_t label_end = info->label_num++;

	info->label_true = label_body;
	info->label_false = label_end;
	info->label_break = label_end;
	info->label_continue = label_body;

	to_code_unconditional_branch(info, label_condition);
	to_code_label(info, label_condition);

	info->variable_location = LFREE;
	const node condition = statement_while_get_condition(nd);
	emit_expression(info, &condition);

	check_type_and_branch(info);

	to_code_label(info, label_body);

	const node body = statement_while_get_body(nd);
	emit_statement(info, &body);

	to_code_unconditional_branch(info, label_condition);
	to_code_label(info, label_end);

	info->label_true = old_label_true;
	info->label_false = old_label_false;
	info->label_break = old_label_break;
	info->label_continue = old_label_continue;
}

/**
 *	Emit do statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_do_statement(information *const info, const node *const nd)
{
	const item_t old_label_true = info->label_true;
	const item_t old_label_false = info->label_false;
	const item_t old_label_break = info->label_break;
	const item_t old_label_continue = info->label_continue;
	const item_t label_loop = info->label_num++;
	const item_t label_end = info->label_num++;

	info->label_true = label_loop;
	info->label_false = label_end;
	info->label_break = label_end;
	info->label_continue = label_loop;

	to_code_unconditional_branch(info, label_loop);
	to_code_label(info, label_loop);

	const node body = statement_do_get_body(nd);
	emit_statement(info, &body);

	info->variable_location = LFREE;
	const node condition = statement_do_get_condition(nd);
	emit_expression(info, &condition);

	check_type_and_branch(info);

	to_code_label(info, label_end);

	info->label_true = old_label_true;
	info->label_false = old_label_false;
	info->label_break = old_label_break;
	info->label_continue = old_label_continue;
}

/**
 *	Emit for statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_for_statement(information *const info, const node *const nd)
{
	// TODO: проверялось, только если в for присутствуют все блоки: инициализация, условие, модификация
	const item_t old_label_true = info->label_true;
	const item_t old_label_false = info->label_false;
	const item_t old_label_break = info->label_break;
	const item_t old_label_continue = info->label_continue;
	const item_t label_condition = info->label_num++;
	const item_t label_body = info->label_num++;
	const item_t label_incr = info->label_num++;
	const item_t label_end = info->label_num++;

	info->label_true = label_body;
	info->label_false = label_end;
	info->label_break = label_end;
	info->label_continue = label_body;

	if (statement_for_has_inition(nd))
	{
		// TODO: рассмотреть случаи, если тут объявление
		const node inition = statement_for_get_inition(nd);
		emit_statement(info, &inition);
	}

	to_code_unconditional_branch(info, label_condition);
	to_code_label(info, label_condition);

	if (statement_for_has_condition(nd))
	{
		const node condition = statement_for_get_condition(nd);
		emit_expression(info, &condition);
	}
	// TODO: проверить разные типы условий: const, reg
	check_type_and_branch(info);

	to_code_label(info, label_incr);
	if (statement_for_has_increment(nd))
	{
		const node increment = statement_for_get_increment(nd);
		emit_expression(info, &increment);
	}

	to_code_unconditional_branch(info, label_condition);
	to_code_label(info, label_body);

	const node body = statement_for_get_body(nd);
	emit_statement(info, &body);

	to_code_unconditional_branch(info, label_incr);
	to_code_label(info, label_end);

	info->label_true = old_label_true;
	info->label_false = old_label_false;
	info->label_break = old_label_break;
	info->label_continue = old_label_continue;
}

/**
 *	Emit return statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_return_statement(information *const info, const node *const nd)
{
	if (info->was_dynamic)
	{
		to_code_stack_load(info, -1);
	}

	if (info->is_main)
	{
		return;
	}

	if (statement_return_has_expression(nd))
	{
		info->variable_location = LREG;
		const node expression = statement_return_get_expression(nd);
		emit_expression(info, &expression);

		// TODO: добавить обработку других ответов (ALOGIC)
		const item_t answer_type = expression_get_type(&expression);
		if (info->answer_kind == ACONST && type_is_integer(info->sx, answer_type))
		{
			uni_printf(info->sx->io, " ret i32 %" PRIitem "\n", info->answer_const);
		}
		else if (info->answer_kind == ACONST && type_is_floating(answer_type))
		{
			uni_printf(info->sx->io, " ret double %f\n", info->answer_const_double);
		}
		else if (info->answer_kind == AREG)
		{
			uni_printf(info->sx->io, " ret ");
			type_to_io(info, answer_type);
			uni_printf(info->sx->io, " %%.%zu\n", info->answer_reg);
		}
	}
	else
	{
		uni_printf(info->sx->io, " ret void\n");
	}
}

/**
 *	Emit translation unit
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_declaration_statement(information *const info, const node *const nd)
{
	const size_t size = statement_declaration_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = statement_declaration_get_declarator(nd, i);
		emit_declaration(info, &decl, true);
	}
}

/**
 *	Emit statement
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static void emit_statement(information *const info, const node *const nd)
{
	switch (statement_get_class(nd))
	{
		case STMT_DECL:
			emit_declaration_statement(info, nd);
			return;

		case STMT_LABEL:
			emit_labeled_statement(info, nd);
			return;

		case STMT_CASE:
			// TODO: case statement emission
			// emit_case_statement(info, nd);
			return;

		case STMT_DEFAULT:
			// TODO: default statement emission
			// emit_default_statement(info, nd);
			return;

		case STMT_COMPOUND:
			emit_compound_statement(info, nd, false);
			return;

		case STMT_EXPR:
			emit_expression(info, nd);
			return;

		case STMT_NULL:
			return;

		case STMT_IF:
			emit_if_statement(info, nd);
			return;

		case STMT_SWITCH:
			// TODO: switch statement emission
			// emit_switch_statement(info, nd);
			return;

		case STMT_WHILE:
			emit_while_statement(info, nd);
			return;

		case STMT_DO:
			emit_do_statement(info, nd);
			return;

		case STMT_FOR:
			emit_for_statement(info, nd);
			return;

		case STMT_GOTO:
			to_code_unconditional_branch(info, (item_t)statement_goto_get_label(nd));
			return;

		case STMT_CONTINUE:
			to_code_unconditional_branch(info, info->label_continue);
			return;

		case STMT_BREAK:
			to_code_unconditional_branch(info, info->label_break);
			return;

		case STMT_RETURN:
			emit_return_statement(info, nd);
			return;

		// Printid и Getid, которые будут сделаны парсере
		default:
			return;
	}
}

/**
 *	Emit translation unit
 *
 *	@param	info		Encoder
 *	@param	nd			Node in AST
 */
static int emit_translation_unit(information *const info, const node *const nd)
{
	const size_t size = translation_unit_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = translation_unit_get_declaration(nd, i);
		emit_declaration(info, &decl, false);
	}

	// FIXME: если это тоже объявление функций, почему тут, а не в functions_declaration?
	if (info->was_stack_functions)
	{
		uni_printf(info->sx->io, "declare i8* @llvm.stacksave()\n");
		uni_printf(info->sx->io, "declare void @llvm.stackrestore(i8*)\n");
	}

	if (info->was_file)
	{
		uni_printf(info->sx->io, "%%struct._IO_FILE = type { i32, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, i8*, "
			"%%struct._IO_marker*, %%struct._IO_FILE*, i32, i32, i64, i16, i8, [1 x i8], i8*, i64, i8*, i8*, i8*, i8*, "
			"i64, i32, [20 x i8] }\n");
		uni_printf(info->sx->io, "%%struct._IO_marker = type { %%struct._IO_marker*, %%struct._IO_FILE*, i32 }\n");
	}

	if (info->was_abs)
	{
		uni_printf(info->sx->io, "declare i32 @abs(i32)\n");
	}

	if (info->was_fabs)
	{
		uni_printf(info->sx->io, "declare double @llvm.fabs.f64(double)\n");
	}


	#ifdef _MSC_VER
		uni_printf(info->sx->io, "!llvm.linker.options = !{!0}\n");
		uni_printf(info->sx->io, "!0 = !{!\"/STACK:268435456\"}\n");
	#endif

	return info->sx->was_error;
}

static void architecture(const workspace *const ws, syntax *const sx)
{
	for (size_t i = 0; ; i++)
	{
		const char *flag = ws_get_flag(ws, i);

		if (flag == NULL || strcmp(flag, "--x86_64") == 0)
		{
			uni_printf(sx->io, "target datalayout = \"e-m:e-i64:64-f80:128-n8:16:32:64-S128\"\n");
			uni_printf(sx->io, "target triple = \"x86_64-pc-linux-gnu\"\n\n");
			return;
		}
		else if (strcmp(flag, "--mipsel") == 0)
		{
			uni_printf(sx->io, "target datalayout = \"e-m:m-p:32:32-i8:8:32-i16:16:32-i64:64-n32-S64\"\n");
			uni_printf(sx->io, "target triple = \"mipsel\"\n\n");
			return;
		}
	}
}

static void structs_declaration(information *const info)
{
	const size_t types = vector_size(&info->sx->types);
	for (size_t i = BEGIN_USER_TYPE; i < types; i++)
	{
		if (type_is_structure(info->sx, (item_t)i))
		{
			uni_printf(info->sx->io, "%%struct_opt.%zu = type { ", i);

			const size_t fields = type_structure_get_member_amount(info->sx, (item_t)i);
			for (size_t j = 0; j < fields; j++)
			{
				uni_printf(info->sx->io, j == 0 ? "" : ", ");
				type_to_io(info, type_structure_get_member_type(info->sx, (item_t)i, j));
			}

			uni_printf(info->sx->io, " }\n");
		}
	}
	uni_printf(info->sx->io, " \n");
}

static void strings_declaration(information *const info)
{
	const size_t amount = strings_amount(info->sx);
	for (size_t i = 0; i < amount; i++)
	{
		const char *string = string_get(info->sx, i);
		const size_t length = strings_length(info->sx, i);
		uni_printf(info->sx->io, "@.str%zu = private unnamed_addr constant [%zu x i8] c\""
			, i, length + 1);

		for (size_t j = 0; j < length; j++)
		{
			const char ch = string[j];
			if (ch == '\n')
			{
				uni_printf(info->sx->io, "\\0A");
			}
			else
			{
				uni_printf(info->sx->io, "%c", ch);
			}
		}
		uni_printf(info->sx->io, "\\00\", align 1\n");
	}
	uni_printf(info->sx->io, " \n");
}


static void builin_functions_declaration(information *const info)
{
	for (size_t i = 0; i < BEGIN_USER_FUNC; i++)
	{
		// Пропускаем, так как эта функция не библиотечная, а реализована вручную в кодах llvm
		if (i == BI_ASSERT || i == BI_PRINT || i == BI_PRINTID || i == BI_GETID)
		{
			continue;
		}

		if (info->was_function[i])
		{
			const item_t func_type = ident_get_type(info->sx, i);
			const item_t ret_type = type_function_get_return_type(info->sx, func_type);
			const size_t parameters = type_function_get_parameter_amount(info->sx, func_type);

			uni_printf(info->sx->io, "declare ");
			type_to_io(info, ret_type);
			uni_printf(info->sx->io, " @%s(", ident_get_spelling(info->sx, i));

			for (size_t j = 0; j < parameters; j++)
			{
				uni_printf(info->sx->io, j == 0 ? "" : ", ");
				type_to_io(info, type_function_get_parameter_type(info->sx, func_type, j));
			}
			uni_printf(info->sx->io, ")\n");
		}
	}
}

// TODO: возможно, тут стоит читать из файла. Или как вообще красиво это сделать?
//  И надо добавлять другие встроенные функции сюда
static void runtime(information *const info)
{
	// assert
	uni_printf(info->sx->io, "@.str = private unnamed_addr constant [3 x i8] c\"%%s\\00\", align 1\n"
		"define void @assert(i32, i8*) {\n"
		" %%3 = alloca i32, align 4\n"
		" %%4 = alloca i8*, align 8\n"
		" store i32 %%0, i32* %%3, align 4\n"
		" store i8* %%1, i8** %%4, align 8\n"
		" %%5 = load i32, i32* %%3, align 4\n"
		" %%6 = icmp ne i32 %%5, 0\n"
		" br i1 %%6, label %%10, label %%7\n"
		" ; <label>:7:                                      ; preds = %%2\n"
		" %%8 = load i8*, i8** %%4, align 8\n"
		" %%9 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([3 x i8], [3 x i8]* @.str, i32 0, i32 0), i8* %%8)\n"
		" call void @exit(i32 1)\n"
		" unreachable\n"
		"; <label>:10:                                     ; preds = %%2\n"
		" ret void\n"
		"}\n"
		"declare void @exit(i32)\n\n");

	// TODO: тут пока заглушки
	// print
	uni_printf(info->sx->io, "define void @print(...) {\n"
		" ret void\n"
		"}\n");

	// printid
	uni_printf(info->sx->io, "define void @printid(...) {\n"
		" ret void\n"
		"}\n\n");
	info->was_function[BI_PRINTF] = true;

	// getid
	uni_printf(info->sx->io, "define void @getid(...) {\n"
		" ret void\n"
		"}\n\n");
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


int encode_to_llvm(const workspace *const ws, syntax *const sx)
{
	if (!ws_is_correct(ws) || sx == NULL)
	{
		return -1;
	}

	information info;
	info.sx = sx;
	info.register_num = 1;
	info.label_num = 1;
	info.init_num = 1;
	info.block_num = 1;
	info.variable_location = LREG;
	info.request_reg = 0;
	info.answer_reg = 0;
	info.was_stack_functions = false;
	info.was_dynamic = false;
	info.was_file = false;
	info.was_abs = false;
	info.was_fabs = false;
	info.is_main = false;
	for (size_t i = 0; i < BEGIN_USER_FUNC; i++)
	{
		info.was_function[i] = false;
	}

	info.arrays = hash_create(HASH_TABLE_SIZE);

	architecture(ws, sx);
	structs_declaration(&info);
	strings_declaration(&info);
	runtime(&info);

	// TODO: нормальное получение корня
	const node root = node_get_root(&info.sx->tree);
	const int ret = emit_translation_unit(&info, &root);
	builin_functions_declaration(&info);

	hash_clear(&info.arrays);
	return ret;
}
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

#include "builder.h"
#include "AST.h"
#include <string.h>


#define MAX_PRINTF_ARGS 20


/**
 *	Emit a semantic error
 *
 *	@param	bldr		AST builder
 *	@param	loc			Error location
 *	@param	num			Error code
 */
static void semantic_error(builder *const bldr, const location loc, err_t num, ...)
{
	va_list args;
	va_start(args, num);

	report_error(&bldr->sx->rprt, bldr->sx->io, loc, num, args);

	va_end(args);
}

static item_t usual_arithmetic_conversions(node *const LHS, node *const RHS)
{
	const item_t LHS_type = expression_get_type(LHS);
	const item_t RHS_type = expression_get_type(RHS);

	if (type_is_floating(LHS_type) || type_is_floating(RHS_type))
	{
		*LHS = build_cast_expression(TYPE_FLOATING, LHS);
		*RHS = build_cast_expression(TYPE_FLOATING, RHS);

		return TYPE_FLOATING;
	}

	return TYPE_INTEGER;
}

static node fold_unary_expression(builder *const bldr, const item_t type, const category_t ctg
	, node *const expr, const unary_t op, const location loc)
{
	if (expression_get_class(expr) != EXPR_LITERAL)
	{
		return expression_unary(type, ctg, expr, op, loc);
	}

	switch (type_get_class(bldr->sx, expression_get_type(expr)))
	{
		case TYPE_NULL_POINTER:
			node_remove(expr);
			return build_boolean_literal_expression(bldr, true, loc);

		case TYPE_BOOLEAN:
		{
			const bool value = expression_literal_get_boolean(expr);
			node_remove(expr);
			return build_boolean_literal_expression(bldr, !value, loc);
		}

		case TYPE_ENUM:
		case TYPE_INTEGER:
		{
			const item_t value = expression_literal_get_integer(expr);
			node_remove(expr);

			switch (op)
			{
				case UN_MINUS:
					return build_integer_literal_expression(bldr, -value, loc);
				case UN_NOT:
					return build_integer_literal_expression(bldr, ~value, loc);
				case UN_LOGNOT:
					return build_boolean_literal_expression(bldr, value == 0, loc);
				case UN_ABS:
					return build_integer_literal_expression(bldr, value >= 0 ? value : -value, loc);
				default:
					return node_broken();
			}
		}

		case TYPE_FLOATING:
		{
			const double value = expression_literal_get_floating(expr);
			node_remove(expr);

			switch (op)
			{
				case UN_MINUS:
					return build_floating_literal_expression(bldr, -value, loc);
				case UN_ABS:
					return build_floating_literal_expression(bldr, value >= 0 ? value : -value, loc);
				default:
					return node_broken();
			}
		}

		default:
			return expression_unary(type, ctg, expr, op, loc);
	}
}

static node fold_binary_expression(builder *const bldr, const item_t type
	, node *const LHS, node *const RHS, const binary_t op, const location loc)
{
	if (expression_get_class(LHS) != EXPR_LITERAL || expression_get_class(RHS) != EXPR_LITERAL)
	{
		return expression_binary(type, LHS, RHS, op, loc);
	}

	switch (type_get_class(bldr->sx, expression_get_type(LHS)))
	{
		case TYPE_ENUM:
		case TYPE_INTEGER:
		{
			const item_t left_value = expression_literal_get_integer(LHS);
			const item_t right_value = expression_literal_get_integer(RHS);
			node_remove(LHS);
			node_remove(RHS);

			switch (op)
			{
				case BIN_MUL:
					return build_integer_literal_expression(bldr, left_value * right_value, loc);
				case BIN_DIV:
					return build_integer_literal_expression(bldr, left_value / right_value, loc);
				case BIN_REM:
					return build_integer_literal_expression(bldr, left_value % right_value, loc);
				case BIN_ADD:
					return build_integer_literal_expression(bldr, left_value + right_value, loc);
				case BIN_SUB:
					return build_integer_literal_expression(bldr, left_value - right_value, loc);
				case BIN_SHL:
					return build_integer_literal_expression(bldr, left_value << right_value, loc);
				case BIN_SHR:
					return build_integer_literal_expression(bldr, left_value >> right_value, loc);
				case BIN_LT:
					return build_boolean_literal_expression(bldr, left_value < right_value, loc);
				case BIN_GT:
					return build_boolean_literal_expression(bldr, left_value > right_value, loc);
				case BIN_LE:
					return build_boolean_literal_expression(bldr, left_value <= right_value, loc);
				case BIN_GE:
					return build_boolean_literal_expression(bldr, left_value >= right_value, loc);
				case BIN_EQ:
					return build_boolean_literal_expression(bldr, left_value == right_value, loc);
				case BIN_NE:
					return build_boolean_literal_expression(bldr, left_value != right_value, loc);
				case BIN_AND:
					return build_integer_literal_expression(bldr, left_value & right_value, loc);
				case BIN_XOR:
					return build_integer_literal_expression(bldr, left_value ^ right_value, loc);
				case BIN_OR:
					return build_integer_literal_expression(bldr, left_value | right_value, loc);
				case BIN_LOG_AND:
					return build_boolean_literal_expression(bldr, left_value && right_value, loc);
				case BIN_LOG_OR:
					return build_boolean_literal_expression(bldr, left_value || right_value, loc);
				default:
					return node_broken();
			}
		}

		case TYPE_FLOATING:
		{
			const double left_value = expression_literal_get_floating(LHS);
			const double right_value = expression_literal_get_floating(RHS);

			node_remove(LHS);
			node_remove(RHS);

			switch (op)
			{
				case BIN_MUL:
					return build_floating_literal_expression(bldr, left_value * right_value, loc);
				case BIN_DIV:
					return build_floating_literal_expression(bldr, left_value / right_value, loc);
				case BIN_ADD:
					return build_floating_literal_expression(bldr, left_value + right_value, loc);
				case BIN_SUB:
					return build_floating_literal_expression(bldr, left_value - right_value, loc);
				case BIN_LT:
					return build_boolean_literal_expression(bldr, left_value < right_value, loc);
				case BIN_GT:
					return build_boolean_literal_expression(bldr, left_value > right_value, loc);
				case BIN_LE:
					return build_boolean_literal_expression(bldr, left_value <= right_value, loc);
				case BIN_GE:
					return build_boolean_literal_expression(bldr, left_value >= right_value, loc);
				case BIN_EQ:
					return build_boolean_literal_expression(bldr, left_value == right_value, loc);
				case BIN_NE:
					return build_boolean_literal_expression(bldr, left_value != right_value, loc);
				default:
					return node_broken();
			}
		}

		default:
			return expression_binary(type, LHS, RHS, op, loc);
	}
}


static size_t evaluate_args(builder *const bldr, const node *const format_str
	, item_t *const format_types, char32_t *const placeholders)
{
	const size_t str_index = expression_literal_get_string(format_str); 
	const char *const string = string_get(bldr->sx, str_index); 

	size_t args = 0;
	for (size_t i = 0; string[i] != '\0'; i += utf8_symbol_size(string[i]))
	{
		if (utf8_convert(&string[i]) == '%')
		{
			i += utf8_symbol_size(string[i]);
			const char32_t specifier = utf8_convert(&string[i]);
			if (specifier != '%')
			{
				if (args == MAX_PRINTF_ARGS)
				{
					semantic_error(bldr, node_get_location(format_str), too_many_printf_args, (size_t)MAX_PRINTF_ARGS);
					return 0;
				}

				placeholders[args] = specifier;
			}

			switch (specifier)
			{
				case 'i':
				case U'ц':
				case 'c':
				case U'л':
					format_types[args++] = TYPE_INTEGER;
					break;

				case 'f':
				case U'в':
					format_types[args++] = TYPE_FLOATING;
					break;

				case 's':
				case U'с':
					format_types[args++] = type_string(bldr->sx);
					break;

				case '%':
					break;

				case '\0':
					semantic_error(bldr, node_get_location(format_str), expected_format_specifier);
					return 0;

				default:
					semantic_error(bldr, node_get_location(format_str), unknown_format_specifier, specifier);
					return 0;
			}
		}
	}

	return args;
}

static node build_printf_expression(builder *const bldr, node *const callee, node_vector *const args, const location r_loc)
{
	const size_t argc = node_vector_size(args);
	if (args == NULL || argc == 0)
	{
		semantic_error(bldr, r_loc, printf_fst_not_string);
		return node_broken();
	}

	if (argc >= MAX_PRINTF_ARGS)
	{
		semantic_error(bldr, r_loc, too_many_printf_args);
		return node_broken();
	}

	const node fst = node_vector_get(args, 0);
	if (expression_get_class(&fst) != EXPR_LITERAL || !type_is_string(bldr->sx, expression_get_type(&fst)))
	{
		semantic_error(bldr, node_get_location(&fst), printf_fst_not_string);
		return node_broken();
	}

	char32_t placeholders[MAX_PRINTF_ARGS];
	item_t format_types[MAX_PRINTF_ARGS];
	const size_t expected_args = evaluate_args(bldr, &fst, format_types, placeholders); 

	if (expected_args != argc - 1)
	{
		semantic_error(bldr, r_loc, wrong_printf_argument_amount);
		return node_broken();
	}

	for (size_t i = 1; i < argc; i++)
	{
		node argument = node_vector_get(args, i);
		if (!check_assignment_operands(bldr, format_types[i - 1], &argument))
		{
			// FIXME: кинуть другую ошибку
			return node_broken();
		}

		node_vector_set(args, i, &argument);
	}

	const location loc = { node_get_location(callee).begin, r_loc.end };
	return expression_call(TYPE_INTEGER, callee, args, loc);
}

static void concat_strings(char *dst, const char *src)
{
	dst = (char *)realloc(dst, strlen(dst) + strlen(src));
	if (!dst)
	{
		printf("realloc error\n"); // какая ошибка?
		return;
	}
	strcat(dst, src);
}

static node create_printf_node(builder *bldr, const char *str, node_vector *args, location l_loc, location r_loc)
{
	const location loc = {l_loc.begin, r_loc.end};

	// создаём строку и вносим её в вектор
	size_t str_index = string_add_by_char(bldr->sx, str);  
	node str_node = build_string_literal_expression(bldr, str_index, loc);

	// создаём новый вектор узлов, чтобы первым аргументом был форматный строковый литерал
	node_vector tmp_node_vector = node_vector_create(); 
	node_vector_add(&tmp_node_vector, &str_node);  

	const size_t argc = node_vector_size(args);
	for (size_t i = 0; i < argc; i++)
	{
		node argument = node_vector_get(args, i);
		node_vector_add(&tmp_node_vector, &argument);
	}

	//node printf_callee = build_identifier_expression(bldr, BI_PRINTF, loc);
	node printf_callee = expression_identifier(&bldr->context, type_function(bldr->sx, TYPE_INTEGER, "s."), BI_PRINTF, loc);  			 
 
	// разворачиваемся в вызов printf
	node call_printf_node = build_printf_expression(bldr, &printf_callee, &tmp_node_vector, r_loc); 

	return call_printf_node;
}

static const char *create_scalar_type_str(item_t type)
{ 	
	switch (type)
	{
		case TYPE_BOOLEAN:
		case TYPE_INTEGER:  
			return "%i";
		case TYPE_CHARACTER:
			return "%c"; 
		case TYPE_FLOATING:
			return "%f"; 
	} 
	return 0;
}

static void to_string(char* restrict buf, int num) 
{  
    if (buf!=NULL) 
        sprintf(buf, "%d", num); 
} 

static char *create_new_temp_identifier(size_t ident_table_size)
{
	char *new_identifier_number_str = (char *)malloc(100);
	if (!new_identifier_number_str)
	{
		printf("malloc error\n");
		return 0;
	}
	to_string(new_identifier_number_str, ident_table_size); 

	char *new_identifier_name = (char *)malloc(1);
	if (!new_identifier_name)
	{
		printf("malloc error\n");
		return 0;
	}
	concat_strings(new_identifier_name, "_temporal_identifier_");
	if (!new_identifier_name) 
		return 0;
	concat_strings(new_identifier_name, new_identifier_number_str);
	if (!new_identifier_name)  
		return 0;  
	concat_strings(new_identifier_name, "_");
	if (!new_identifier_name)  
		return 0;  
	
	return new_identifier_name;
}   

static node create_struct_nodes(builder *bldr, node *argument, size_t tab_deep, location l_loc, location r_loc);
 
static node create_array_nodes(builder *bldr, node *argument, location l_loc, location r_loc, size_t dimensions)
{    
	const location loc = {l_loc.begin, r_loc.end}; 

	item_t type = expression_get_type(argument);
 
	// сначала -- запоминание аргумента, потом -- цикл

	// будет statement_compound'ом
	node result;

	node_vector res_stmts = node_vector_create();  // для аргументов результата-statement compound'а
	node_vector body_args = node_vector_create();  // для построения тела цикла 

	// сохраняем переданный аргумент в новую переменную-идентификатор с уникальным именем
	char *arg_identifier_name = create_new_temp_identifier((size_t)vector_size(&bldr->sx->identifiers));
	if (!arg_identifier_name)
	{
		printf("unable to create new identifier\n");
		return node_broken();
	} 
	// добавляем имя нового идентификатора в таблицу representations 
	const size_t arg_repr = map_add(&bldr->sx->representations, arg_identifier_name, ITEM_MAX);  
	// добавляем идентификатор в identifiers
	const size_t arg_id = ident_add(bldr->sx, arg_repr, 0, type, 3);  
	/*
	// где-то здесь ошибка?
	node arg_parent = node_add_child(&bldr->context, OP_DECLSTMT);
	node arg_nd = node_add_child(&arg_parent, OP_DECL_VAR);
	node_add_arg(&arg_nd, (item_t)arg_id);
	node_add_arg(&arg_nd, 0);	// Размерность
	node_add_arg(&arg_nd, 0);	// Флаг наличия инициализатора
	node_set_arg(&arg_nd, 1, (item_t)dimensions);
	node_set_arg(&arg_nd, 2, false);

	// запоминаем объявление
	node_vector_add(&res_stmts, &arg_parent);  
	*/ 
	// присваивание
	node tmp_arg1 = build_identifier_expression(bldr, arg_repr, loc);
	node bin = build_binary_expression(bldr, &tmp_arg1, argument, BIN_ASSIGN, loc); 
	node_vector_add(&res_stmts, &bin);   
	
	item_t elements_type = type_array_get_element_type(bldr->sx, type); 
	if (type_is_array(bldr->sx, elements_type))
	{   
		// 1. инициализация -- объявляем новый уникальный идентификатор     
		char *new_identifier_name = create_new_temp_identifier((size_t)vector_size(&bldr->sx->identifiers));
		if (!new_identifier_name)
		{
			printf("unable to create new identifier\n");
			return node_broken();
		} 
		// добавляем имя нового идентификатора в таблицу representations 
		const size_t repr = map_add(&bldr->sx->representations, new_identifier_name, ITEM_MAX);  
		// добавляем идентификатор в identifiers
		const size_t id = ident_add(bldr->sx, repr, 0, TYPE_INTEGER, 3); 

		node init_expr = node_add_child(&bldr->context, OP_DECLSTMT);
		node nd = node_add_child(&init_expr, OP_DECL_VAR);
		node_add_arg(&nd, (item_t)id);
		node_add_arg(&nd, 0);	// Размерность
		node_add_arg(&nd, 0);	// Флаг наличия инициализатора
		node_set_arg(&nd, 2, true);

		node init_rhs_expr = build_integer_literal_expression(bldr, 0, loc); 
		node temp = node_add_child(&nd, OP_NOP);
		node_swap(&init_rhs_expr, &temp);
		node_remove(&temp);    

		// 2. условие
		node cond_lhs_expr = build_identifier_expression(bldr, repr, loc); 

		// главный узел, который отправится в build_unary_expression() оператора upb 
		node cond_main = build_identifier_expression(bldr, arg_repr, loc); 

		// оператор upb ("кол_во")
		node cond_rhs_expr = build_unary_expression(bldr, &cond_main, UN_UPB, loc);

		node cond_expr = build_binary_expression(bldr, &cond_lhs_expr, &cond_rhs_expr, BIN_LT, loc);

		// 3. инкремент
		node incr_lhs_expr = build_identifier_expression(bldr, repr, loc); 
		node incr_expr = build_unary_expression(bldr, &incr_lhs_expr, UN_POSTINC, loc);

		// 4. тело цикла   
		node tmp_arg2 = build_identifier_expression(bldr, arg_repr, loc);
		node main_subscript_index = build_identifier_expression(bldr, repr, loc);
		node main_subscript = build_subscript_expression(bldr, &tmp_arg2, &main_subscript_index, l_loc, r_loc);  
 
		node array_nodes = create_array_nodes(bldr, &main_subscript, l_loc, r_loc, dimensions-1);   

		// разворачиваемся в узлы printf, чтобы отпечатать "{" и "}, " (или "}", если цикл дальше не пойдёт)
		node_vector blank_tmp_node_vector = node_vector_create();
		node first_printf_node = create_printf_node(bldr, "{", &blank_tmp_node_vector, l_loc, r_loc); 
		node_vector_add(&body_args, &first_printf_node); 

		node_vector_add(&body_args, &array_nodes);
		
		// левая часть if
		node if_binary_lhs = build_identifier_expression(bldr, repr, loc);

		// правая часть if: "upb(<массив по нужному измерению>) - 1"
 
		// главный узел, который отправится в build_unary_expression() оператора upb 
		node if_binary_rhs_arg = build_identifier_expression(bldr, arg_repr, loc); 

		// оператор upb ("кол_во")
		node if_binary_rhs_upb = build_unary_expression(bldr, &if_binary_rhs_arg, UN_UPB, loc); 

		node if_binary_rhs_num_one = build_integer_literal_expression(bldr, 1, loc);

		// оператор "минус"
		node if_binary_rhs = build_binary_expression(bldr, &if_binary_rhs_upb, &if_binary_rhs_num_one, BIN_SUB, loc);

		// операция сравнения (равенства)
		node if_binary = build_binary_expression(bldr, &if_binary_lhs, &if_binary_rhs, BIN_EQ, loc);

		// условие выполнено
		node if_true = create_printf_node(bldr, "}", &blank_tmp_node_vector, l_loc, r_loc);
		// условие не выполнено
		node if_false = create_printf_node(bldr, "}, ", &blank_tmp_node_vector, l_loc, r_loc);

		// само выражение
		node if_statement = build_if_statement(bldr, &if_binary, &if_true, &if_false, loc); 
		node_vector_add(&body_args, &if_statement);

		node body = build_compound_statement(bldr, &body_args, l_loc, r_loc);
		
		// 5. полный узел цикла
		node for_stmt = build_for_statement(bldr, &init_expr, &cond_expr, &incr_expr, &body, loc);  

		node_vector_add(&res_stmts, &for_stmt);

		result = build_compound_statement(bldr, &res_stmts, l_loc, r_loc);
		return result;
	}   
	 
	// 1. инициализация -- объявляем новый уникальный идентификатор     
	char *new_identifier_name = create_new_temp_identifier((size_t)vector_size(&bldr->sx->identifiers));
	if (!new_identifier_name)
	{
		printf("unable to create new identifier\n");
		return node_broken();
	} 
	// добавляем имя нового идентификатора в таблицу representations 
	const size_t repr = map_add(&bldr->sx->representations, new_identifier_name, ITEM_MAX);  
	// добавляем идентификатор в identifiers
	const size_t id = ident_add(bldr->sx, repr, 0, TYPE_INTEGER, 3); 

	node init_expr = node_add_child(&bldr->context, OP_DECLSTMT);
	node nd = node_add_child(&init_expr, OP_DECL_VAR);
	node_add_arg(&nd, (item_t)id);
	node_add_arg(&nd, 0);	// Размерность
	node_add_arg(&nd, 0);	// Флаг наличия инициализатора
	node_set_arg(&nd, 2, true);

	node init_rhs_expr = build_integer_literal_expression(bldr, 0, loc); 
	node temp = node_add_child(&nd, OP_NOP);
	node_swap(&init_rhs_expr, &temp);
	node_remove(&temp);    

	// 2. условие
	node cond_lhs_expr = build_identifier_expression(bldr, repr, loc); 
	
	// узел, который отправится в build_unary_expression() оператора upb 
	node tmp_arg3 = build_identifier_expression(bldr, arg_repr, loc);   
	// оператор upb ("кол_во")
	node cond_rhs_expr = build_unary_expression(bldr, &tmp_arg3, UN_UPB, loc);  

	node cond_expr = build_binary_expression(bldr, &cond_lhs_expr, &cond_rhs_expr, BIN_LT, loc);

	// 3. инкремент
	node incr_lhs_expr = build_identifier_expression(bldr, repr, loc); 
	node incr_expr = build_unary_expression(bldr, &incr_lhs_expr, UN_POSTINC, loc);
	 
	// 4. тело цикла (зависит от класса типа элементов массива)
	item_t type_class = type_get_class(bldr->sx, elements_type);
	if (type_class != TYPE_STRUCTURE)
	{  
		// разворачиваемся в узел if, в котором будет printf, для этого строим строки для каждого из случаев
		char *str = (char *)malloc(1);
		if (!str)
		{
			printf("malloc error\n");
			return node_broken();
		} 
		const char *tmp = create_scalar_type_str(type_class);
		if (!tmp)
			return node_broken();
		concat_strings(str, tmp);
		if (!str)
			return node_broken(); 

		// создаём узлы вырезки, которые отправятся в качестве аргумента для printf  
		// узлов должно быть два, т.к. будем разворачиваться в if statement

		node tmp_arg4 = build_identifier_expression(bldr, arg_repr, loc);
		node main_subscript1_index = build_identifier_expression(bldr, repr, loc);
		node main_subscript1 = build_subscript_expression(bldr, &tmp_arg4, &main_subscript1_index, l_loc, r_loc);

		node_vector tmp_args_nv1 = node_vector_create();
		node_vector_add(&tmp_args_nv1, &main_subscript1); 
 
		node tmp_arg5 = build_identifier_expression(bldr, arg_repr, loc); 
		node main_subscript2_index = build_identifier_expression(bldr, repr, loc);
		node main_subscript2 = build_subscript_expression(bldr, &tmp_arg5, &main_subscript2_index, l_loc, r_loc); 

		node_vector tmp_args_nv2 = node_vector_create();
		node_vector_add(&tmp_args_nv2, &main_subscript2);  
		
		// левая часть if
		node if_binary_lhs = build_identifier_expression(bldr, repr, loc);

		// правая часть if: "upb(<массив по нужному измерению>) - 1"
 
		// узел, который отправится в build_unary_expression() оператора upb 
		node tmp_arg6 = build_identifier_expression(bldr, arg_repr, loc); 

		// оператор upb ("кол_во")
		node if_binary_rhs_upb = build_unary_expression(bldr, &tmp_arg6, UN_UPB, loc); 

		node if_binary_rhs_num_one = build_integer_literal_expression(bldr, 1, loc);

		// оператор "минус"
		node if_binary_rhs = build_binary_expression(bldr, &if_binary_rhs_upb, &if_binary_rhs_num_one, BIN_SUB, loc);

		// операция сравнения (равенства)
		node if_binary = build_binary_expression(bldr, &if_binary_lhs, &if_binary_rhs, BIN_EQ, loc);

		// разворачиваемся в узлы printf, чтобы отпечатать "%<нужный идентификатор>, " или "%<нужный идентификатор>", если цикл дальше не пойдёт
		// условие выполнено
		node if_true = create_printf_node(bldr, str, &tmp_args_nv1, l_loc, r_loc);
		// условие не выполнено
		concat_strings(str, ", ");
		node if_false = create_printf_node(bldr, str, &tmp_args_nv2, l_loc, r_loc);

		// само выражение
		node if_statement = build_if_statement(bldr, &if_binary, &if_true, &if_false, loc); 

		node_vector_add(&body_args, &if_statement);

		node body = build_compound_statement(bldr, &body_args, l_loc, r_loc); 

		// 5. полный узел цикла
		node for_stmt = build_for_statement(bldr, &init_expr, &cond_expr, &incr_expr, &body, loc); 

		node_vector_add(&res_stmts, &for_stmt);

		result = build_compound_statement(bldr, &res_stmts, l_loc, r_loc);

		return result;
	}
	else 
	{ 
		// создаём узел структуры как корректную вырезку из массива
		node tmp_arg7 = build_identifier_expression(bldr, arg_repr, loc); 
		node main_subscript2_index = build_identifier_expression(bldr, repr, loc);
		node subs_struct_node = build_subscript_expression(bldr, &tmp_arg7, &main_subscript2_index, l_loc, r_loc);  

		// разворачиваемся в узел printf, чтобы отпечатать "\n{ struct" 
		node_vector blank_tmp_node_vector = node_vector_create();
		node first_printf_node = create_printf_node(bldr, "\n{ struct", &blank_tmp_node_vector, l_loc, r_loc);
		node_vector_add(&body_args, &first_printf_node);
		
		// отправляем нужный узел в create_struct_nodes()
		node struct_node = create_struct_nodes(bldr, &subs_struct_node, 1, l_loc, r_loc); 
		node_vector_add(&body_args, &struct_node);  

		// if-statement: будет отпечатывать запятую после каждого элемента массива, кроме последнего

		// левая часть if
		node if_binary_lhs = build_identifier_expression(bldr, repr, loc);

		// правая часть if: "upb(<массив по нужному измерению>) - 1"
  
		node tmp_arg8 = build_identifier_expression(bldr, arg_repr, loc);  
		// оператор upb ("кол_во")
		node if_binary_rhs_upb = build_unary_expression(bldr, &tmp_arg8, UN_UPB, loc); 

		node if_binary_rhs_num_one = build_integer_literal_expression(bldr, 1, loc);

		// оператор "минус"
		node if_binary_rhs = build_binary_expression(bldr, &if_binary_rhs_upb, &if_binary_rhs_num_one, BIN_SUB, loc);

		// операция сравнения (равенства)
		node if_binary = build_binary_expression(bldr, &if_binary_lhs, &if_binary_rhs, BIN_EQ, loc);

		// условие выполнено
		node if_true = create_printf_node(bldr, "\n}", &blank_tmp_node_vector, l_loc, r_loc);
		// условие не выполнено
		node if_false = create_printf_node(bldr, "}, ", &blank_tmp_node_vector, l_loc, r_loc);

		// само выражение
		node if_statement = build_if_statement(bldr, &if_binary, &if_true, &if_false, loc); 
		node_vector_add(&body_args, &if_statement); 

		node body = build_compound_statement(bldr, &body_args, l_loc, r_loc); 

		// 5. полный узел цикла
		node for_stmt = build_for_statement(bldr, &init_expr, &cond_expr, &incr_expr, &body, loc); 

		node_vector_add(&res_stmts, &for_stmt);

		result = build_compound_statement(bldr, &res_stmts, l_loc, r_loc);

		return result;
	}

}

static void create_correct_spaces(char *str, size_t tab_deep)
{ 
	// глубина одного уровня -- 4 пробела
	for (size_t j = 0; j < tab_deep*4; j++)
	{
		concat_strings(str, " ");
		if (!str)
			return;
	}
}

static node create_struct_nodes(builder *bldr, node *argument, size_t tab_deep, location l_loc, location r_loc)
{
	const location loc = {l_loc.begin, r_loc.end};

	// будет statement compound'ом
	node result;

	node_vector res_stmts = node_vector_create(); // для аргументов результата-statement compound'а

	item_t type = expression_get_type(argument);

	// сохраняем переданный аргумент в новую переменную-идентификатор с уникальным именем
	char *arg_identifier_name = create_new_temp_identifier((size_t)vector_size(&bldr->sx->identifiers));
	if (!arg_identifier_name)
	{
		printf("unable to create new identifier\n");
		return node_broken();
	} 
	// добавляем имя нового идентификатора в таблицу representations 
	const size_t arg_repr = map_add(&bldr->sx->representations, arg_identifier_name, ITEM_MAX-1);  
	// добавляем идентификатор в identifiers 
	const size_t arg_id = ident_add(bldr->sx, arg_repr, 0, type, 3); 
	 
	node arg_parent = node_add_child(&bldr->context, OP_DECLSTMT);
	node arg_nd = node_add_child(&arg_parent, OP_DECL_VAR);
	node_add_arg(&arg_nd, (item_t)arg_id);
	node_add_arg(&arg_nd, 0);	// Размерность
	node_add_arg(&arg_nd, 0);	// Флаг наличия инициализатора
	node_set_arg(&arg_nd, 2, true);  
 
	node arg_temp = node_add_child(&arg_nd, OP_NOP);
	node_swap(argument, &arg_temp);
	node_remove(&arg_temp);

	// запоминаем объявление
	node_vector_add(&res_stmts, &arg_parent);  
 
	const size_t member_amount = type_structure_get_member_amount(bldr->sx, type); 
	for (size_t i = 0; i < member_amount; i++)
	{
		item_t member_type = type_structure_get_member_type(bldr->sx, type, i);
		item_t member_type_class = type_get_class(bldr->sx, member_type);
		size_t member_name = type_structure_get_member_name(bldr->sx, type, i);
		const char *member_name_str = repr_get_name(bldr->sx, member_name); 

		// создаём узел идентификатора для переданного argument  
		// строим узел поля структуры
		node tmp_arg2 = build_identifier_expression(bldr, arg_repr, loc);
		node member_node = build_member_expression(bldr, &tmp_arg2, member_name, 0, l_loc, r_loc); 

		// создаём корректное начало строки
		char *str = (char *)malloc(1);
		if (!str)
		{
			printf("malloc error\n");
			return node_broken();
		}
		concat_strings(str, "\n\n");
		if (!str)
			return node_broken();
		create_correct_spaces(str, tab_deep);
		if (!str)
			return node_broken();
		concat_strings(str, "{\n");
		if (!str)
			return node_broken();
		create_correct_spaces(str, tab_deep+1);
		if (!str)
			return node_broken();
		concat_strings(str, ".");
		if (!str)
			return node_broken();
		concat_strings(str, member_name_str);
		if (!str)
			return node_broken();
		concat_strings(str, " = ");
		if (!str)
			return node_broken();

		if ((member_type_class == TYPE_ARRAY) || (member_type_class == TYPE_STRUCTURE))
		{ 
			node_vector blank_tmp_nv = node_vector_create();

			if (member_type_class == TYPE_ARRAY)
			{    
				size_t dimensions = 1;
				
				item_t elements_type = type_array_get_element_type(bldr->sx, member_type); 
				while (type_is_array(bldr->sx, elements_type)) 
				{
					dimensions += 1;
					elements_type = type_array_get_element_type(bldr->sx, elements_type);
				}

				node printf_str_node = create_printf_node(bldr, str, &blank_tmp_nv, l_loc, r_loc);
				node_vector_add(&res_stmts, &printf_str_node); 
				node printf_paren_first = create_printf_node(bldr, "{", &blank_tmp_nv, l_loc, r_loc); 
				node_vector_add(&res_stmts, &printf_paren_first); 
 
				node array_node = create_array_nodes(bldr, &member_node, l_loc, r_loc, dimensions);
				node_vector_add(&res_stmts, &array_node); 
				 
				node printf_paren_second = create_printf_node(bldr, "}", &blank_tmp_nv, l_loc, r_loc); 
				node_vector_add(&res_stmts, &printf_paren_second); 
			}
			else
			{
				const char *temp_str = "{ struct";
				//concat_strings(str, temp_str); 
				if (!str)
					return node_broken(); 
				node printf_str_node = create_printf_node(bldr, str, &blank_tmp_nv, l_loc, r_loc); 
				node printf_tmp_node = create_printf_node(bldr, temp_str, &blank_tmp_nv, l_loc, r_loc);
				node_vector_add(&res_stmts, &printf_str_node);  
				node_vector_add(&res_stmts, &printf_tmp_node);  

				node struct_node = create_struct_nodes(bldr, &member_node, tab_deep+1, l_loc, r_loc);
				node_vector_add(&res_stmts, &struct_node); 
				
				node printf_paren_second = create_printf_node(bldr, " }", &blank_tmp_nv, l_loc, r_loc);
				node_vector_add(&res_stmts, &printf_paren_second); 
			}
		}
		else
		{ 
			// для скалярных типов разворачиваемся в узлы printf по отдельности

			const char *tmp = create_scalar_type_str(member_type_class);   
			if (!tmp)
				return node_broken(); 
			concat_strings(str, tmp);
			if (!str)
				return node_broken(); 

			node_vector tmp_nv = node_vector_create();
			node_vector_add(&tmp_nv, &member_node);

			node printf_node = create_printf_node(bldr, str, &tmp_nv, l_loc, r_loc);

			node_vector_add(&res_stmts, &printf_node);

		}  
		free(str);
		// правильно завершаем строку: если текущее поле, которое требуется распечатать, -- последнее, то запятую не ставим, иначе -- ставим
		str = (char *)malloc(1);
		if (!str)
		{
			printf("malloc error\n");
			return node_broken();
		}
		concat_strings(str, "\n");
		if (!str)
			return node_broken();
		create_correct_spaces(str, tab_deep);
		if (!str)
			return node_broken();
		(i == member_amount-1) ? concat_strings(str, "}") : concat_strings(str, "},");
		if (!str)
			return node_broken();

		node_vector tmp_nv = node_vector_create();
		node printf_node = create_printf_node(bldr, str, &tmp_nv, l_loc, r_loc);

		node_vector_add(&res_stmts, &printf_node);
	}

	result = build_compound_statement(bldr, &res_stmts, l_loc, r_loc);
	return result; 
}

static node build_print_expression(builder *const bldr, node *const callee, node_vector *const args, const location r_loc)
{
	const location loc = { node_get_location(callee).begin, r_loc.end };    
	
	node_vector stmts = node_vector_create();   

	node_vector_add(&stmts, callee);
		
	node_vector tmp_args = node_vector_create();

	size_t argc;
	if (args == NULL || (argc = node_vector_size(args)) == 0)
	{
		semantic_error(bldr, r_loc, expected_expression);
		return node_broken();
	} 

	bool complicated_type_in_args = 0;  

	size_t last_scalar_argument_index = 0;
	size_t first_scalar_argument_index = 0;
	size_t last_argument_index = 0;

	char *str = (char *) malloc(1); 
	if (!str)
	{
		printf("malloc error\n"); // какая ошибка?
		return node_broken();
	} 
	
	for (size_t i = 0; i < argc; i++)
	{  
		node argument = node_vector_get(args, i);

		if (type_is_pointer(bldr->sx, expression_get_type(&argument)))
		{
			semantic_error(bldr, node_get_location(&argument), pointer_in_print);
			return node_broken();
		}  

		location curr_loc = node_get_location(&argument); 
		
		node last_argument = *callee;
		if (i > 0)
		{
			last_argument_index = i - 1;
			last_argument = node_vector_get(args, last_argument_index);
		} 
		location last_argument_loc = node_get_location(&last_argument);  
		
		const item_t argument_type = expression_get_type(&argument);
		const item_t argument_type_class = type_get_class(bldr->sx, argument_type); 
		if (argument_type_class == TYPE_ARRAY || argument_type_class == TYPE_STRUCTURE)
		{ 
			complicated_type_in_args = 1;  
 
			(argument_type_class == TYPE_ARRAY) ? concat_strings(str, "{") : concat_strings(str, "{ struct"); 
			if (!str)
				return node_broken();   

			// корректно определяем location	
			node first_scalar_argument = node_vector_get(args, first_scalar_argument_index);
			location fs_arg_loc = node_get_location(&first_scalar_argument);
			node last_scalar_argument = node_vector_get(args, last_scalar_argument_index);
			location ls_arg_loc = node_get_location(&last_scalar_argument);

			// разворачиваемся в printf для имеющейся на данный момент строки
			node printf_node = create_printf_node(bldr, str, &tmp_args, fs_arg_loc, ls_arg_loc); 
			// запоминаем узел 
			node_vector_add(&stmts, &printf_node);

			// дальнейшая строка и аргументы не будут иметь к только что построенному узлу никакого отношения
			free(str); 
			str = (char *)malloc(1);
			if (!str)
			{
				printf("malloc error\n");
				return node_broken();
			}
			// избавляемся от предыдущих запомненных аргументов 
			tmp_args = node_vector_create(); 
			first_scalar_argument_index = 0; 
			last_scalar_argument_index = 0;  
			
			if (argument_type_class == TYPE_ARRAY) 
			{
				size_t dimensions = 1;
				
				item_t elements_type = type_array_get_element_type(bldr->sx, argument_type); 
				while (type_is_array(bldr->sx, elements_type)) 
				{
					dimensions += 1;
					elements_type = type_array_get_element_type(bldr->sx, elements_type);
				}

				node complicated_type_node = create_array_nodes(bldr, &argument,  last_argument_loc, curr_loc, dimensions); 
				concat_strings(str, "} ");
				node_vector_add(&stmts, &complicated_type_node);
			}
			else
			{
				node complicated_type_node = create_struct_nodes(bldr, &argument, 1, last_argument_loc, curr_loc);
				node_vector_add(&stmts, &complicated_type_node);
				concat_strings(str, "\n}\n");
			} 
			if (!str)
				return node_broken();
		}
		else
		{ 
			// запоминаем для дальнейшего использования аргумент только в случае, если тип скалярный
			node_vector_add(&tmp_args, &argument);
 
			if (last_scalar_argument_index == 0)
				first_scalar_argument_index = i;

			last_scalar_argument_index = i; 

			const char *tmp = create_scalar_type_str(argument_type_class);   
			if (!tmp)
				return node_broken(); 
			concat_strings(str, tmp);
			if (!str)
				return node_broken();
			concat_strings(str, " ");
			if (!str)
				return node_broken();
		} 

	}  
	node last_argument = node_vector_get(args, first_scalar_argument_index);
	location last_argument_loc = node_get_location(&last_argument); 

	// разворачиваемся в printf для имеющейся на данный момент строки 
	node printf_node = create_printf_node(bldr, str, &tmp_args, last_argument_loc, r_loc); 
 
	if (!complicated_type_in_args)
	{
		node_vector_clear(&stmts);
		node_remove(callee);
		return printf_node;
	}

	node_vector_add(&stmts, &printf_node); 
		 
	return expression_inline(TYPE_VOID, &stmts, loc); 
} 

static node build_printid_expression(builder *const bldr, node *const callee, node_vector *const args, const location r_loc)
{ 
	const location loc = { node_get_location(callee).begin, r_loc.end };   
	
	node_vector stmts = node_vector_create();   
	node_vector_add(&stmts, callee);

	node_vector tmp_args = node_vector_create();

	const size_t argc = node_vector_size(args);
	if (args == NULL || argc == 0)
	{
		semantic_error(bldr, r_loc, expected_identifier_in_printid);
		return node_broken();
	} 

	bool complicated_type_in_args = 0;    

	size_t last_scalar_argument_index = 0;
	size_t first_scalar_argument_index = 0;
	size_t last_argument_index = 0;
	
	char *str = (char *) malloc(1); 
	if (!str)
	{
		printf("malloc error\n"); // какая ошибка?
		return node_broken();
	}
 
	for (size_t i = 0; i < argc; i++)
	{
		node argument = node_vector_get(args, i);
		if (node_is_correct(&argument) && expression_get_class(&argument) != EXPR_IDENTIFIER)
		{
			semantic_error(bldr, node_get_location(&argument), expected_identifier_in_printid);
			return node_broken();
		}  

		location curr_loc = node_get_location(&argument); 
 
		node last_argument = *callee;
		if (i > 0)
		{
			last_argument_index = i - 1;
			last_argument = node_vector_get(args, last_argument_index);
		} 
		location last_argument_loc = node_get_location(&last_argument); 

		// создаём строку "имя_аргумента = "
		char *argument_name = (char *)malloc(1);
		if (!argument_name)
		{
			printf("malloc error\n"); // какая ошибка?
			return node_broken();
		}
		const size_t argument_identifier_index = expression_identifier_get_id(&argument);
		const char *tmp_argument_name = ident_get_spelling(bldr->sx, argument_identifier_index); 
		concat_strings(argument_name, tmp_argument_name);
		if (!argument_name)
			return node_broken();
		concat_strings(argument_name, " = ");
		if (!argument_name)
			return node_broken();

		// соединяем с основной строкой
		concat_strings(str, argument_name);			
		if (!str)
			return node_broken();
		
		const item_t argument_type = expression_get_type(&argument);
		const item_t argument_type_class = type_get_class(bldr->sx, argument_type); 
		if (argument_type_class == TYPE_ARRAY || argument_type_class == TYPE_STRUCTURE)
		{  
			complicated_type_in_args = 1;   
 
			(argument_type_class == TYPE_ARRAY) ? concat_strings(str, "{") : concat_strings(str, "{ struct");  
			if (!str)
				return node_broken();

			// корректно определяем location
			node first_scalar_argument = node_vector_get(args, first_scalar_argument_index);
			location fs_arg_loc = node_get_location(&first_scalar_argument); 
			node last_scalar_argument = node_vector_get(args, last_scalar_argument_index);
			location ls_arg_loc = node_get_location(&last_scalar_argument); 

			// разворачиваемся в printf для имеющейся на данный момент строки
			node printf_node = create_printf_node(bldr, str, &tmp_args, fs_arg_loc, ls_arg_loc); 
			// запоминаем узел 
			node_vector_add(&stmts, &printf_node);
	
			// дальнейшая строка и аргументы не будут иметь к только что построенному узлу никакого отношения
			free(str); 
			str = (char *)malloc(1);
			if (!str)
			{
				printf("malloc error\n");
				return node_broken();
			}
			// избавляемся от предыдущих добавленных аргументов 
			tmp_args = node_vector_create(); 
			first_scalar_argument_index = 0; 
			last_scalar_argument_index = 0; 
 
			if (argument_type_class == TYPE_ARRAY) 
			{
				size_t dimensions = 1;
				
				item_t elements_type = type_array_get_element_type(bldr->sx, argument_type); 
				while (type_is_array(bldr->sx, elements_type)) 
				{
					dimensions += 1;
					elements_type = type_array_get_element_type(bldr->sx, elements_type);
				}

				node complicated_type_node = create_array_nodes(bldr, &argument,  last_argument_loc, curr_loc, dimensions); 
				concat_strings(str, "} ");
				node_vector_add(&stmts, &complicated_type_node);
			}
			else
			{
				node complicated_type_node = create_struct_nodes(bldr, &argument, 1, last_argument_loc, curr_loc);
				node_vector_add(&stmts, &complicated_type_node);
				concat_strings(str, "\n}\n");
			} 
			if (!str)
				return node_broken();
		}
		else
		{ 
			// запоминаем для возможного использования аргумент только в случае, если тип скалярный
			node_vector_add(&tmp_args, &argument);
			
			if (last_scalar_argument_index == 0)
				first_scalar_argument_index = i;

			last_scalar_argument_index = i;

			const char *tmp = create_scalar_type_str(argument_type_class);   
			if (!tmp)
				return node_broken(); 
			concat_strings(str, tmp);
			if (!str)
				return node_broken();
			concat_strings(str, " ");
			if (!str)
				return node_broken();
		} 	 
	}  

	node last_argument = node_vector_get(args, first_scalar_argument_index);
	location last_argument_loc = node_get_location(&last_argument);

	// разворачиваемся в printf для имеющейся на данный момент строки
	node printf_node = create_printf_node(bldr, str, &tmp_args, last_argument_loc, r_loc);
 
	if (!complicated_type_in_args)
	{
		node_vector_clear(&stmts);
		node_remove(callee);
		return printf_node;
	}

	node_vector_add(&stmts, &printf_node); 

	return expression_inline(TYPE_VOID, &stmts, loc);
}

static node build_getid_expression(builder *const bldr, node *const callee, node_vector *const args, const location r_loc)
{
	const size_t argc = node_vector_size(args);
	if (args == NULL || argc == 0)
	{
		semantic_error(bldr, r_loc, expected_identifier_in_getid);
		return node_broken();
	}

	for (size_t i = 0; i < argc; i++)
	{
		const node argument = node_vector_get(args, i);
		if (node_is_correct(&argument) && expression_get_class(&argument) != EXPR_IDENTIFIER)
		{
			semantic_error(bldr, node_get_location(&argument), expected_identifier_in_getid);
			return node_broken();
		}
	}

	const location loc = { node_get_location(callee).begin, r_loc.end };
	return expression_call(TYPE_VOID, callee, args, loc);
}


/*
 *	 __     __   __     ______   ______     ______     ______   ______     ______     ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\    \ \_\  \ \_____\  \ \_\ \_\  \ \_\    \ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/     \/_/   \/_____/   \/_/ /_/   \/_/     \/_/\/_/   \/_____/   \/_____/
 */


builder builder_create(syntax *const sx)
{
	builder bldr;
	bldr.sx = sx;

	node root = node_get_root(&bldr.sx->tree);
	node_copy(&bldr.context, &root);

	return bldr;
}


bool check_assignment_operands(builder *const bldr, const item_t expected_type, node *const init)
{
	if (!node_is_correct(init))
	{
		return true;
	}

	syntax *const sx = bldr->sx;
	const location loc = node_get_location(init);
	if (expression_get_class(init) == EXPR_INITIALIZER)
	{
		const size_t actual_inits = expression_initializer_get_size(init);
		if (type_is_structure(sx, expected_type))
		{
			const size_t expected_inits = type_structure_get_member_amount(sx, expected_type);
			if (expected_inits != actual_inits)
			{
				semantic_error(bldr, loc, wrong_init_in_actparam, expected_inits, actual_inits);
				return false;
			}

			for (size_t i = 0; i < actual_inits; i++)
			{
				const item_t type = type_structure_get_member_type(sx, expected_type, i);
				node subexpr = expression_initializer_get_subexpr(init, i);
				if (!check_assignment_operands(bldr, type, &subexpr))
				{
					return false;
				}
			}

			expression_initializer_set_type(init, expected_type);
			return true;
		}
		else if (type_is_array(sx, expected_type))
		{
			const item_t type = type_array_get_element_type(sx, expected_type);
			for (size_t i = 0; i < actual_inits; i++)
			{
				node subexpr = expression_initializer_get_subexpr(init, i);
				if (!check_assignment_operands(bldr, type, &subexpr))
				{
					return false;
				}
			}

			expression_initializer_set_type(init, expected_type);
			return true;
		}
		else
		{
			semantic_error(bldr, loc, wrong_init);
			return false;
		}
	}

	const item_t actual_type = expression_get_type(init);
	if (type_is_floating(expected_type) && type_is_integer(sx, actual_type))
	{
		*init = build_cast_expression(expected_type, init);
		return true;
	}

	if (type_is_enum(sx, expected_type) && type_is_enum_field(sx, actual_type))
	{
		return true; // check enum initializer
	}

	if (type_is_integer(sx, expected_type) && (type_is_enum(sx, actual_type) || type_is_enum_field(sx, actual_type)))
	{
		return true; // check int initializer
	}

	if (type_is_integer(sx, expected_type) && type_is_integer(sx, actual_type))
	{
		return true;
	}

	if (type_is_pointer(sx, expected_type) && type_is_null_pointer(actual_type))
	{
		return true;
	}

	if (expected_type == actual_type)
	{
		return true;
	}

	semantic_error(bldr, loc, wrong_init);
	return false;
}

node build_identifier_expression(builder *const bldr, const size_t name, const location loc)
{
	const item_t identifier = repr_get_reference(bldr->sx, name);

	if (identifier == ITEM_MAX)
	{
		semantic_error(bldr, loc, use_of_undeclared_identifier, repr_get_name(bldr->sx, name));
		return node_broken();
	}

	const item_t type = ident_get_type(bldr->sx, (size_t)identifier);
	if (type_is_enum_field(bldr->sx, type))
	{
		const item_t enum_type = get_enum_field_type(bldr->sx, type);
		const item_t value = ident_get_displ(bldr->sx, (size_t)identifier);
		return expression_integer_literal(&bldr->context, enum_type, value, loc);
	}

	return expression_identifier(&bldr->context, type, (size_t)identifier, loc);
}

node build_null_literal_expression(builder *const bldr, const location loc)
{
	return expression_null_literal(&bldr->context, TYPE_NULL_POINTER, loc);
}

node build_boolean_literal_expression(builder *const bldr, const bool value, const location loc)
{
	return expression_boolean_literal(&bldr->context, TYPE_BOOLEAN, value, loc);
}

node build_character_literal_expression(builder *const bldr, const char32_t value, const location loc)
{
	return expression_character_literal(&bldr->context, TYPE_CHARACTER, value, loc);
}

node build_integer_literal_expression(builder *const bldr, const item_t value, const location loc)
{
	return expression_integer_literal(&bldr->context, TYPE_INTEGER, value, loc);
}

node build_floating_literal_expression(builder *const bldr, const double value, const location loc)
{
	return expression_floating_literal(&bldr->context, TYPE_FLOATING, value, loc);
}

node build_string_literal_expression(builder *const bldr, const size_t index, const location loc)
{
	return expression_string_literal(&bldr->context, type_string(bldr->sx), index, loc);
}

node build_subscript_expression(builder *const bldr, node *const base, node *const index
	, const location l_loc, const location r_loc)
{
	if (!node_is_correct(base) || !node_is_correct(index))
	{
		return node_broken();
	}

	const item_t base_type = expression_get_type(base);
	if (!type_is_array(bldr->sx, base_type))
	{
		semantic_error(bldr, l_loc, subscripted_expr_not_array);
		return node_broken();
	}

	const item_t index_type = expression_get_type(index);
	if (!type_is_integer(bldr->sx, index_type))
	{
		semantic_error(bldr, node_get_location(index), array_subscript_not_integer);
		return node_broken();
	}

	const item_t element_type = type_array_get_element_type(bldr->sx, base_type);

	const location loc = { node_get_location(base).begin, r_loc.end };
	return expression_subscript(element_type, base, index, loc);
}

node build_call_expression(builder *const bldr, node *const callee
	, node_vector *const args, const location l_loc, const location r_loc)
{
	if (!node_is_correct(callee))
	{
		return node_broken();
	}

	const item_t callee_type = expression_get_type(callee);
	if (!type_is_function(bldr->sx, callee_type))
	{
		semantic_error(bldr, l_loc, called_expr_not_function);
		return node_broken();
	}

	if (expression_get_class(callee) == EXPR_IDENTIFIER)
	{
		switch (expression_identifier_get_id(callee))
		{
			case BI_PRINTF:
				return build_printf_expression(bldr, callee, args, r_loc);
			case BI_PRINT:
				return build_print_expression(bldr, callee, args, r_loc);
			case BI_PRINTID:
				return build_printid_expression(bldr, callee, args, r_loc);
			case BI_GETID:
				return build_getid_expression(bldr, callee, args, r_loc);
			default:
				break;
		}
	}

	const size_t expected_args = type_function_get_parameter_amount(bldr->sx, callee_type);
	const size_t actual_args = args != NULL ? node_vector_size(args) : 0;

	if (expected_args != actual_args)
	{
		semantic_error(bldr, r_loc, wrong_argument_amount, expected_args, actual_args);
		return node_broken();
	}

	for (size_t i = 0; i < actual_args; i++)
	{
		const item_t expected_type = type_function_get_parameter_type(bldr->sx, callee_type, i);
		node argument = node_vector_get(args, i);
		if (!check_assignment_operands(bldr, expected_type, &argument))
		{
			return node_broken();
		}

		node_vector_set(args, i, &argument);
	}

	const item_t return_type = type_function_get_return_type(bldr->sx, callee_type);
	const location loc = { node_get_location(callee).begin, r_loc.end };
	return expression_call(return_type, callee, args, loc);
}

node build_member_expression(builder *const bldr, node *const base, const size_t name
	, const bool is_arrow, const location op_loc, const location id_loc)
{
	if (!node_is_correct(base))
	{
		return node_broken();
	}

	const item_t base_type = expression_get_type(base);
	item_t struct_type;
	category_t category;

	if (!is_arrow)
	{
		if (!type_is_structure(bldr->sx, base_type))
		{
			semantic_error(bldr, op_loc, member_reference_not_struct);
			return node_broken();
		}

		struct_type = base_type;
		category = expression_is_lvalue(base) ? LVALUE : RVALUE;
	}
	else
	{
		if (!type_is_struct_pointer(bldr->sx, base_type))
		{
			semantic_error(bldr, op_loc, member_reference_not_struct_pointer);
			return node_broken();
		}

		struct_type = type_pointer_get_element_type(bldr->sx, base_type);
		category = LVALUE;
	}

	const size_t member_amount = type_structure_get_member_amount(bldr->sx, struct_type);
	for (size_t i = 0; i < member_amount; i++)
	{
		if (name == type_structure_get_member_name(bldr->sx, struct_type, i))
		{
			const item_t type = type_structure_get_member_type(bldr->sx, struct_type, i);
			const location loc = { node_get_location(base).begin, id_loc.end };

			return expression_member(type, category, i, is_arrow, base, loc);
		}
	}

	semantic_error(bldr, id_loc, no_such_member, repr_get_name(bldr->sx, name));
	return node_broken();
}

node build_cast_expression(const item_t target_type, node *const expr)
{
	if (!node_is_correct(expr))
	{
		return node_broken();
	}

	const item_t source_type = expression_get_type(expr);
	const location loc = node_get_location(expr);

	if (target_type != source_type)
	{
		if (expression_get_class(expr) == EXPR_LITERAL)
		{
			// Пока тут только int -> float
			const item_t value = expression_literal_get_integer(expr);
			const node result = node_insert(expr, OP_LITERAL, DOUBLE_SIZE + 4);

			node_set_arg(&result, 0, TYPE_FLOATING);
			node_set_arg(&result, 1, RVALUE);
			node_set_arg_double(&result, 2, (double)value);
			node_set_arg(&result, DOUBLE_SIZE + 2, (item_t)loc.begin);
			node_set_arg(&result, DOUBLE_SIZE + 3, (item_t)loc.end);

			node_remove(expr);
			return result;
		}

		return expression_cast(target_type, source_type, expr, loc);
	}

	return *expr;
}

node build_unary_expression(builder *const bldr, node *const operand, const unary_t op_kind, const location op_loc)
{
	if (!node_is_correct(operand))
	{
		return node_broken();
	}

	const item_t operand_type = expression_get_type(operand);

	const location loc = op_kind == UN_POSTINC || op_kind == UN_POSTDEC
		? (location){ node_get_location(operand).begin, op_loc.end }
		: (location){ op_loc.begin, node_get_location(operand).end };

	switch (op_kind)
	{
		case UN_POSTINC:
		case UN_POSTDEC:
		case UN_PREINC:
		case UN_PREDEC:
		{
			if (!expression_is_lvalue(operand))
			{
				semantic_error(bldr, op_loc, unassignable_expression);
				return node_broken();
			}

			if (!type_is_arithmetic(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, increment_operand_not_arithmetic, op_kind);
				return node_broken();
			}

			return expression_unary(operand_type, RVALUE, operand, op_kind, loc);
		}

		case UN_ADDRESS:
		{
			if (!expression_is_lvalue(operand))
			{
				semantic_error(bldr, op_loc, addrof_operand_not_lvalue);
				return node_broken();
			}

			const item_t type = type_pointer(bldr->sx, operand_type);
			return expression_unary(type, RVALUE, operand, op_kind, loc);
		}

		case UN_INDIRECTION:
		{
			if (!type_is_pointer(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, indirection_operand_not_pointer);
				return node_broken();
			}

			const item_t type = type_pointer_get_element_type(bldr->sx, operand_type);
			return expression_unary(type, LVALUE, operand, op_kind, loc);
		}

		case UN_ABS:
		case UN_MINUS:
		{
			if (!type_is_arithmetic(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, unary_operand_not_arithmetic, operand_type);
				return node_broken();
			}

			return fold_unary_expression(bldr, operand_type, RVALUE, operand, op_kind, loc);
		}

		case UN_NOT:
		{
			if (!type_is_integer(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, unnot_operand_not_integer, operand_type);
				return node_broken();
			}

			return fold_unary_expression(bldr, TYPE_INTEGER, RVALUE, operand, op_kind, loc);
		}

		case UN_LOGNOT:
		{
			if (!type_is_scalar(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, lognot_operand_not_scalar, operand_type);
				return node_broken();
			}

			return fold_unary_expression(bldr, TYPE_BOOLEAN, RVALUE, operand, op_kind, loc);
		}

		case UN_UPB:
		{
			if (!type_is_array(bldr->sx, operand_type))
			{
				semantic_error(bldr, op_loc, upb_operand_not_array, operand_type);
				return node_broken();
			}

			return fold_unary_expression(bldr, TYPE_INTEGER, RVALUE, operand, op_kind, loc);
		}

		default:
			// Unknown unary operator
			return node_broken();
	}
}

node build_binary_expression(builder *const bldr, node *const LHS, node *const RHS
	, const binary_t op_kind, const location op_loc)
{
	if (!node_is_correct(LHS) || !node_is_correct(RHS))
	{
		return node_broken();
	}

	const item_t left_type = expression_get_type(LHS);
	const item_t right_type = expression_get_type(RHS);

	if (operation_is_assignment(op_kind))
	{
		if (!expression_is_lvalue(LHS))
		{
			semantic_error(bldr, op_loc, unassignable_expression);
			return node_broken();
		}

		if (!check_assignment_operands(bldr, left_type, RHS))
		{
			return node_broken();
		}
	}

	const location loc = { node_get_location(LHS).begin, node_get_location(RHS).end };

	switch (op_kind)
	{
		case BIN_REM:
		case BIN_SHL:
		case BIN_SHR:
		case BIN_AND:
		case BIN_XOR:
		case BIN_OR:
		{
			if (!type_is_integer(bldr->sx, left_type) || !type_is_integer(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(bldr, TYPE_INTEGER, LHS, RHS, op_kind, loc);
		}

		case BIN_MUL:
		case BIN_DIV:
		case BIN_ADD:
		case BIN_SUB:
		{
			if (!type_is_arithmetic(bldr->sx, left_type) || !type_is_arithmetic(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			const item_t type = usual_arithmetic_conversions(LHS, RHS);
			return fold_binary_expression(bldr, type, LHS, RHS, op_kind, loc);
		}

		case BIN_LT:
		case BIN_GT:
		case BIN_LE:
		case BIN_GE:
		{
			if (!type_is_arithmetic(bldr->sx, left_type) || !type_is_arithmetic(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			usual_arithmetic_conversions(LHS, RHS);
			return fold_binary_expression(bldr, TYPE_BOOLEAN, LHS, RHS, op_kind, loc);
		}

		case BIN_EQ:
		case BIN_NE:
		{
			if (type_is_floating(left_type) || type_is_floating(right_type))
			{
				warning(bldr->sx->io, variable_deviation);
			}

			if (type_is_arithmetic(bldr->sx, left_type) && type_is_arithmetic(bldr->sx, right_type))
			{
				usual_arithmetic_conversions(LHS, RHS);
				return fold_binary_expression(bldr, TYPE_BOOLEAN, LHS, RHS, op_kind, loc);
			}

			if ((type_is_pointer(bldr->sx, left_type) && type_is_null_pointer(right_type))
				|| (type_is_null_pointer(left_type) && type_is_pointer(bldr->sx, right_type))
				|| left_type == right_type)
			{
				return fold_binary_expression(bldr, TYPE_BOOLEAN, LHS, RHS, op_kind, loc);
			}

			semantic_error(bldr, op_loc, typecheck_binary_expr);
			return node_broken();
		}

		case BIN_LOG_AND:
		case BIN_LOG_OR:
		{
			if (!type_is_scalar(bldr->sx, left_type) || !type_is_scalar(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return fold_binary_expression(bldr, TYPE_BOOLEAN, LHS, RHS, op_kind, loc);
		}

		case BIN_ASSIGN:
			return expression_assignment(left_type, LHS, RHS, op_kind, loc);

		case BIN_REM_ASSIGN:
		case BIN_SHL_ASSIGN:
		case BIN_SHR_ASSIGN:
		case BIN_AND_ASSIGN:
		case BIN_XOR_ASSIGN:
		case BIN_OR_ASSIGN:
		{
			if (!type_is_integer(bldr->sx, left_type) || !type_is_integer(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return expression_assignment(left_type, LHS, RHS, op_kind, loc);
		}

		case BIN_MUL_ASSIGN:
		case BIN_DIV_ASSIGN:
		case BIN_ADD_ASSIGN:
		case BIN_SUB_ASSIGN:
		{
			if (!type_is_arithmetic(bldr->sx, left_type) || !type_is_arithmetic(bldr->sx, right_type))
			{
				semantic_error(bldr, op_loc, typecheck_binary_expr);
				return node_broken();
			}

			return expression_assignment(left_type, LHS, RHS, op_kind, loc);
		}

		case BIN_COMMA:
			return expression_binary(right_type, LHS, RHS, op_kind, loc);

		default:
			// Unknown binary operator
			return node_broken();
	}
}

node build_ternary_expression(builder *const bldr, node *const cond, node *const LHS, node *const RHS, location op_loc)
{
	if (!node_is_correct(cond) || !node_is_correct(LHS) || !node_is_correct(RHS))
	{
		return node_broken();
	}

	if (expression_get_class(LHS) == EXPR_INITIALIZER)
	{
		semantic_error(bldr, node_get_location(LHS), expected_expression);
		return node_broken();
	}

	if (expression_get_class(RHS) == EXPR_INITIALIZER)
	{
		semantic_error(bldr, node_get_location(RHS), expected_expression);
		return node_broken();
	}

	const item_t cond_type = expression_get_type(cond);
	if (!type_is_scalar(bldr->sx, cond_type))
	{
		semantic_error(bldr, node_get_location(cond), condition_must_be_scalar);
		return node_broken();
	}

	const location loc = { node_get_location(cond).begin, node_get_location(RHS).end };

	const item_t LHS_type = expression_get_type(LHS);
	const item_t RHS_type = expression_get_type(RHS);
	if (type_is_arithmetic(bldr->sx, LHS_type) && type_is_arithmetic(bldr->sx, RHS_type))
	{
		const item_t type = usual_arithmetic_conversions(LHS, RHS);
		return expression_ternary(type, cond, LHS, RHS, loc);
	}

	if (type_is_pointer(bldr->sx, LHS_type) && type_is_null_pointer(RHS_type))
	{
		return expression_ternary(LHS_type, cond, LHS, RHS, loc);
	}

	if ((type_is_null_pointer(LHS_type) && type_is_pointer(bldr->sx, RHS_type))
		|| (LHS_type == RHS_type))
	{
		return expression_ternary(RHS_type, cond, LHS, RHS, loc);
	}

	semantic_error(bldr, op_loc, incompatible_cond_operands);
	return node_broken();
}

node build_initializer(builder *const bldr, node_vector *const exprs, const location l_loc, const location r_loc)
{
	const size_t actual_inits = node_vector_size(exprs);
	if (actual_inits == 0)
	{
		semantic_error(bldr, l_loc, empty_init);
		return node_broken();
	}

	const location loc = { l_loc.begin, r_loc.end };
	return expression_initializer(exprs, loc);
}

node build_constant_expression(builder *const bldr, node *const expr)
{
	if (expression_get_class(expr) != EXPR_LITERAL)
	{
		semantic_error(bldr, node_get_location(expr), expected_constant_expression);
		return node_broken();
	}

	return *expr;
}


node build_case_statement(builder *const bldr, node *const expr, node *const substmt, const location case_loc)
{
	if (!node_is_correct(expr) || !node_is_correct(substmt))
	{
		return node_broken();
	}

	if (!type_is_integer(bldr->sx, expression_get_type(expr)))
	{
		semantic_error(bldr, node_get_location(expr), case_expr_not_integer);
		return node_broken();
	}

	const location loc = { case_loc.begin, node_get_location(substmt).end };
	return statement_case(expr, substmt, loc);
}

node build_default_statement(builder *const bldr, node *const substmt, const location default_loc)
{
	if (!node_is_correct(substmt))
	{
		return node_broken();
	}

	(void)bldr;
	const location loc = { default_loc.begin, node_get_location(substmt).end };
	return statement_default(substmt, loc);
}

node build_compound_statement(builder *const bldr, node_vector *const stmts, location l_loc, location r_loc)
{
	if (stmts)
	{
		const size_t amount = node_vector_size(stmts);
		for (size_t i = 0; i < amount; i++)
		{
			node item = node_vector_get(stmts, i);
			if (!node_is_correct(&item))
			{
				return node_broken();
			}
		}
	}

	const location loc = { l_loc.begin, r_loc.end };
	return statement_compound(&bldr->context, stmts, loc);
}

node build_null_statement(builder *const bldr, const location semi_loc)
{
	return statement_null(&bldr->context, semi_loc);
}

node build_if_statement(builder *const bldr, node *const cond
	, node *const then_stmt, node *const else_stmt, const location if_loc)
{
	if (!node_is_correct(cond) || !node_is_correct(then_stmt) || (else_stmt && !node_is_correct(else_stmt)))
	{
		return node_broken();
	}

	if (!type_is_scalar(bldr->sx, expression_get_type(cond)))
	{
		semantic_error(bldr, node_get_location(cond), condition_must_be_scalar);
		return node_broken();
	}

	const location loc = { if_loc.begin, node_get_location(else_stmt ? else_stmt : then_stmt).end };
	return statement_if(cond, then_stmt, else_stmt, loc);
}

node build_switch_statement(builder *const bldr, node *const cond, node *const body, const location switch_loc)
{
	if (!node_is_correct(cond) || !node_is_correct(body))
	{
		return node_broken();
	}

	if (!type_is_integer(bldr->sx, expression_get_type(cond)))
	{
		semantic_error(bldr, node_get_location(cond), switch_expr_not_integer);
		return node_broken();
	}

	const location loc = { switch_loc.begin, node_get_location(body).end };
	return statement_switch(cond, body, loc);
}

node build_while_statement(builder *const bldr, node *const cond, node *const body, const location while_loc)
{
	if (!node_is_correct(cond) || !node_is_correct(body))
	{
		return node_broken();
	}

	if (!type_is_scalar(bldr->sx, expression_get_type(cond)))
	{
		semantic_error(bldr, node_get_location(cond), condition_must_be_scalar);
		return node_broken();
	}

	const location loc = { while_loc.begin, node_get_location(body).end };
	return statement_while(cond, body, loc);
}

node build_do_statement(builder *const bldr, node *const body, node *const cond, const location do_loc)
{
	if (!node_is_correct(body) || !node_is_correct(cond))
	{
		return node_broken();
	}

	if (!type_is_scalar(bldr->sx, expression_get_type(cond)))
	{
		semantic_error(bldr, node_get_location(cond), condition_must_be_scalar);
		return node_broken();
	}

	const location loc = { do_loc.begin, node_get_location(cond).end };
	return statement_do(body, cond, loc);
}

node build_for_statement(builder *const bldr, node *const init
	, node *const cond, node *const incr, node *const body, const location for_loc)
{
	if ((init && !node_is_correct(init)) || (cond && !node_is_correct(cond))
		|| (incr && !node_is_correct(incr)) || !node_is_correct(body))
	{
		return node_broken();
	}

	if (cond && !type_is_scalar(bldr->sx, expression_get_type(cond)))
	{
		semantic_error(bldr, node_get_location(cond), condition_must_be_scalar);
		return node_broken();
	}

	const location loc = { for_loc.begin, node_get_location(body).end };
	return statement_for(init, cond, incr, body, loc);
}

node build_continue_statement(builder *const bldr, const location continue_loc)
{
	return statement_continue(&bldr->context, continue_loc);
}

node build_break_statement(builder *const bldr, const location break_loc)
{
	return statement_break(&bldr->context, break_loc);
}

node build_return_statement(builder *const bldr, node *const expr, const location return_loc)
{
	location loc = return_loc;
	const item_t return_type = type_function_get_return_type(bldr->sx, bldr->func_type);
	if (expr != NULL)
	{
		if (!node_is_correct(expr))
		{
			return node_broken();
		}

		if (type_is_void(return_type))
		{
			semantic_error(bldr, node_get_location(expr), void_func_valued_return);
			return node_broken();
		}

		// TODO: void*?
		if (return_type != type_pointer(bldr->sx, TYPE_VOID))
		{
			check_assignment_operands(bldr, return_type, expr);
		}

		loc.end = node_get_location(expr).end;
	}
	else
	{
		if (!type_is_void(return_type))
		{
			semantic_error(bldr, return_loc, nonvoid_func_void_return);
			return node_broken();
		}
	}

	return statement_return(&bldr->context, expr, loc);
}

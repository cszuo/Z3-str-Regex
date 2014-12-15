#include<stdio.h>
#include<stdlib.h>
#include<string>
#include<stdarg.h>
#include<memory.h>
#include<z3.h>

using namespace std;

/********************************* Auxiliary Function ********************************************/

void exitf(const char* message) 
{
  fprintf(stderr,"BUG: %s.\n", message);
  exit(1);
}

void error_handler(Z3_context ctx, Z3_error_code e) 
{
    printf("Error code: %d\n", e);
    exitf("incorrect use of Z3");
}

Z3_context mk_context_custom(Z3_config cfg, Z3_error_handler err) 
{
    Z3_context ctx;
    
    Z3_set_param_value(cfg, "MODEL", "true");
    ctx = Z3_mk_context(cfg);
#ifdef TRACING
    Z3_trace_to_stderr(ctx);
#endif
    Z3_set_error_handler(ctx, err);
    
    return ctx;
}

Z3_context mk_context() 
{
    Z3_config  cfg;
    Z3_context ctx;
    cfg = Z3_mk_config();
    ctx = mk_context_custom(cfg, error_handler);
    Z3_del_config(cfg);
    return ctx;
}


Z3_ast mk_var(Z3_context ctx, const char * name, Z3_sort ty) 
{
    Z3_symbol   s  = Z3_mk_string_symbol(ctx, name);
    return Z3_mk_const(ctx, s, ty);
}

Z3_ast mk_bool_var(Z3_context ctx, const char * name) 
{
    Z3_sort ty = Z3_mk_bool_sort(ctx);
    return mk_var(ctx, name, ty);
}

Z3_ast mk_int_var(Z3_context ctx, const char * name) 
{
    Z3_sort ty = Z3_mk_int_sort(ctx);
    return mk_var(ctx, name, ty);
}

Z3_ast mk_int(Z3_context ctx, int v) 
{
    Z3_sort ty = Z3_mk_int_sort(ctx);
    return Z3_mk_int(ctx, v, ty);
}


Z3_ast mk_unary_app(Z3_context ctx, Z3_func_decl f, Z3_ast x) 
{
    Z3_ast args[1] = {x};
    return Z3_mk_app(ctx, f, 1, args);
}

Z3_ast mk_binary_app(Z3_context ctx, Z3_func_decl f, Z3_ast x, Z3_ast y) 
{
    Z3_ast args[2] = {x, y};
    return Z3_mk_app(ctx, f, 2, args);
}

void check(Z3_context ctx, Z3_lbool expected_result)
{
    Z3_model m      = 0;
    Z3_lbool result = Z3_check_and_get_model(ctx, &m);
    switch (result) {
    case Z3_L_FALSE:
        printf("unsat\n");
        break;
    case Z3_L_UNDEF:
        printf("unknown\n");
        printf("potential model:\n%s\n", Z3_model_to_string(ctx, m));
        break;
    case Z3_L_TRUE:
        printf("sat\n%s\n", Z3_model_to_string(ctx, m));
        break;
    }
    if (m) {
        Z3_del_model(ctx, m);
    }
    if (result != expected_result) {
        exitf("unexpected result");
    }
}

void display_eqc(Z3_theory t, Z3_ast n) {
    Z3_context c = Z3_theory_get_context(t);
    Z3_ast curr = n;
    printf("  ----- begin eqc of %s", Z3_ast_to_string(c, n));
    printf(", root: %s\n", Z3_ast_to_string(c, Z3_theory_get_eqc_root(t, n)));
    do {
        printf("  %s\n", Z3_ast_to_string(c, curr));
        curr = Z3_theory_get_eqc_next(t, curr);
    }
    while (curr != n);
    printf("  ----- end of eqc\n");
}

void display_eqc_parents(Z3_theory t, Z3_ast n) {
    Z3_context c = Z3_theory_get_context(t);
    Z3_ast curr = n;
    printf("  ----- begin eqc (theory) parents of %s\n", Z3_ast_to_string(c, n));
    do {
        unsigned num_parents = Z3_theory_get_num_parents(t, curr);
        unsigned i;
        for (i = 0; i < num_parents; i++) {
            Z3_ast p = Z3_theory_get_parent(t, curr, i);
            printf("  %s\n", Z3_ast_to_string(c, p));
        }
        curr = Z3_theory_get_eqc_next(t, curr);
    }
    while (curr != n);
    printf("  ----- end of eqc (theory) parents\n");
}

void display_new_eq(Z3_theory t, Z3_ast n1, Z3_ast n2) {
    printf("====== begin new equality\n");
    display_eqc(t, n1);
    display_eqc_parents(t, n1);
    printf("  ==\n");
    display_eqc(t, n2);
    display_eqc_parents(t, n2);
    printf("====== end new equality\n");
}

Z3_ast get_eqc_value(Z3_theory t, Z3_ast n) {
    Z3_ast curr = n;
    do {
        if (Z3_theory_is_value(t, curr))
            return curr;
        curr = Z3_theory_get_eqc_next(t, curr);
    }
    while (curr != n);
    return 0;
}

/************************************* Theory Main code ******************************************/
struct _RegexTheoryData {
    Z3_sort      String; 
    Z3_func_decl Matches;
};

typedef struct _RegexTheoryData RegexTheoryData;

typedef enum
{
  my_Z3_ConstStr,    // 0
  my_Z3_ConstBool,   //
  my_Z3_Func,        //
  my_Z3_Num,         //
  my_Z3_Var,         //
  my_Z3_Str_Var,     //
  my_Z3_Int_Var,     //
  my_Z3_Quantifier,  //
  my_Z3_Unknown      //
} rgTheoryType;

/*
 * Return the type of the Z3_ast
 * Copied from Z3-str
 */
rgTheoryType getNodeType(Z3_theory t, Z3_ast n) {
  	Z3_context ctx = Z3_theory_get_context(t);
  	RegexTheoryData * td = (RegexTheoryData*) Z3_theory_get_ext_data(t);
  	Z3_ast_kind z3Kind = Z3_get_ast_kind(ctx, n);

  	switch (z3Kind) {
		case Z3_NUMERAL_AST: {
		  	return my_Z3_Num;
		  	break;
		}

		case Z3_APP_AST: {
		  	Z3_sort s = Z3_get_sort(ctx, n);
		  	if (Z3_theory_is_value(t, n)) {
		    	Z3_sort_kind sk = Z3_get_sort_kind(ctx, s);
		    	Z3_func_decl d = Z3_get_app_decl(ctx, Z3_to_app(ctx, n));
		    	if (sk == Z3_BOOL_SORT) {
		      		if (d == td->Matches) {
		        		return my_Z3_Func;
		      		} else {
		        		return my_Z3_ConstBool;
		      		}
		    	} else if (sk == Z3_UNKNOWN_SORT) {
		      		if (s == td->String) {
		  				return my_Z3_ConstStr;
		      		}
		    	}
		  	} else {
				//Z3 native functions fall into this category
				Z3_sort s = Z3_get_sort(ctx, n);
				if (s == td->String) {
				  	return my_Z3_Str_Var;
				} else {
				  	return my_Z3_Func;
				}
		 	}
		  	break;
		}

		case Z3_VAR_AST: {
		  	return my_Z3_Var;
		  	break;
		}

		default: {
		  	break;
		}
  	}
  	return my_Z3_Unknown;
}

/*
 * Return the const string value from Z3_ast n
 * Copied from Z3-str
 */
string getConstStrValue(Z3_theory t, Z3_ast n) {
	Z3_context ctx = Z3_theory_get_context(t);
	string strValue;
	if (getNodeType(t, n) == my_Z3_ConstStr) {
		char * str = (char *) Z3_ast_to_string(ctx, n);
		if (strcmp(str, "\"\"") == 0){
			strValue = string("");
		} else {
			strValue = string(str);
		}
	} else {
		strValue = string("__NotConstStr__");
	}
	return strValue;
}

bool regex_matches(string str, string regex){
	//TODO
	return false;
}


/******************************* Procedural Attachment Functións *********************************/

void rg_delete(Z3_theory t) {
    RegexTheoryData * td = (RegexTheoryData *)Z3_theory_get_ext_data(t);
    printf("Delete\n");
    free(td);
}

Z3_bool rg_reduce_app(Z3_theory t, Z3_func_decl d, unsigned n, Z3_ast const args[], Z3_ast * result) {
	Z3_context ctx = Z3_theory_get_context(t);
    RegexTheoryData * td = (RegexTheoryData*)Z3_theory_get_ext_data(t);
    if (d == td->Matches) {
        if (getNodeType(t, args[0]) == my_Z3_ConstStr && getNodeType(t, args[1]) == my_Z3_ConstStr){
			string firstString = getConstStrValue(t, args[0]);
			string regExp 		= getConstStrValue(t, args[1]);
			if (regex_matches(firstString,regExp)){
				* result = Z3_mk_true(ctx);				
			} else {
				* result = Z3_mk_false(ctx);
			}
			return Z3_TRUE;
		}
    }
    return Z3_FALSE; // failed to simplify
}

void rg_new_app(Z3_theory t, Z3_ast n) {
    Z3_context c = Z3_theory_get_context(t);
    printf("New app: %s\n", Z3_ast_to_string(c, n));
}

void rg_new_elem(Z3_theory t, Z3_ast n) {
    Z3_context c = Z3_theory_get_context(t);
    printf("New elem: %s\n", Z3_ast_to_string(c, n));
}

void rg_init_search(Z3_theory t) {
    printf("Starting search\n");
}

void rg_push(Z3_theory t) {
    printf("Push\n");
}

void rg_pop(Z3_theory t) {
    printf("Pop\n");
}

void rg_reset(Z3_theory t) {
    printf("Reset\n");
}

void rg_restart(Z3_theory t) {
    printf("Restart\n");
}

void rg_new_eq(Z3_theory t, Z3_ast n1, Z3_ast n2) {
    Z3_context c = Z3_theory_get_context(t);
    printf("New equality: %s ", Z3_ast_to_string(c, n1));
    printf("!= %s\n", Z3_ast_to_string(c, n2));

    RegexTheoryData * td = (RegexTheoryData*)Z3_theory_get_ext_data(t);
    //TODO    
}

void rg_new_diseq(Z3_theory t, Z3_ast n1, Z3_ast n2) {
    Z3_context c = Z3_theory_get_context(t);
    printf("New disequality: %s ", Z3_ast_to_string(c, n1));
    printf("!= %s\n", Z3_ast_to_string(c, n2));
}

void rg_new_relevant(Z3_theory t, Z3_ast n) {
    Z3_context c = Z3_theory_get_context(t);
    printf("Relevant: %s\n", Z3_ast_to_string(c, n));
}

void rg_new_assignment(Z3_theory t, Z3_ast n, Z3_bool v) {
    Z3_context c = Z3_theory_get_context(t);
    printf("Assigned: %s --> %d\n", Z3_ast_to_string(c, n), v);
}

Z3_bool rg_final_check(Z3_theory t) {
    printf("Final check\n");
    return Z3_TRUE;
}

Z3_theory mk_simple_theory(Z3_context ctx) {
    Z3_sort matchesDomain[2];
    Z3_sort boolSort 		= Z3_mk_bool_sort(ctx);
    Z3_symbol string		= Z3_mk_string_symbol(ctx, "String");
    Z3_symbol matchesFunc	= Z3_mk_string_symbol(ctx, "Matches");

    RegexTheoryData * td 	= (RegexTheoryData*)malloc(sizeof(RegexTheoryData)); 
    Z3_theory theory      	= Z3_mk_theory(ctx, "Regex_theory", td);
    td->String            	= Z3_theory_mk_sort(ctx, theory, string); 
    matchesDomain[0] 		= td->String; 
    matchesDomain[1] 		= td->String;
    td->Matches      		= Z3_theory_mk_func_decl(ctx, theory, matchesFunc, 2, matchesDomain, boolSort);

    Z3_set_delete_callback(theory, rg_delete);
    Z3_set_reduce_app_callback(theory, rg_reduce_app);
    Z3_set_new_app_callback(theory, rg_new_app);
    Z3_set_new_elem_callback(theory, rg_new_elem);
    Z3_set_init_search_callback(theory, rg_init_search);
    Z3_set_push_callback(theory, rg_push);
    Z3_set_pop_callback(theory, rg_pop);
    Z3_set_reset_callback(theory, rg_reset);
    Z3_set_restart_callback(theory, rg_restart);
    Z3_set_new_eq_callback(theory, rg_new_eq);
    Z3_set_new_diseq_callback(theory, rg_new_diseq);
    Z3_set_new_relevant_callback(theory, rg_new_relevant);
    Z3_set_new_assignment_callback(theory, rg_new_assignment);
    Z3_set_final_check_callback(theory, rg_final_check);
    return theory;
}

/*void simple_example1() 
{
    Z3_ast a, b, c, f1, f2, f3, r, i;
    Z3_context ctx;
    Z3_theory Th;
    SimpleTheoryData * td;
    printf("\nsimple_example1\n");
    ctx = mk_context();
    Th = mk_simple_theory(ctx);
    td = (SimpleTheoryData*)Z3_theory_get_ext_data(Th);
    a  = mk_var(ctx, "a", td->S);
    b  = mk_var(ctx, "b", td->S);
    c  = mk_var(ctx, "c", td->S);
    i  = Z3_mk_ite(ctx, Z3_mk_eq(ctx, td->lunit, td->runit), c, td->runit);
    f1 = mk_binary_app(ctx, td->f, a, i);
    f2 = mk_binary_app(ctx, td->f, td->lunit, f1);
    f3 = mk_binary_app(ctx, td->f, b, f2);
    printf("%s\n==>\n", Z3_ast_to_string(ctx, f3));
    r  = Z3_simplify(ctx, f3);
    printf("%s\n",      Z3_ast_to_string(ctx, r));

    Z3_del_context(ctx);
}*/

int main() 
{
    //simple_example1();
    return 0;
}

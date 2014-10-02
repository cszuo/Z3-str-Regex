/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    macro_util.cpp

Abstract:

    Macro finding goodies.
    They are used during preprocessing (MACRO_FINDER=true), and model building.

Author:

    Leonardo de Moura (leonardo) 2010-12-15.

Revision History:

--*/
#include"macro_util.h"
#include"occurs.h"
#include"arith_simplifier_plugin.h"
#include"basic_simplifier_plugin.h"
#include"bv_simplifier_plugin.h"
#include"var_subst.h"
#include"ast_pp.h"
#include"ast_ll_pp.h"
#include"ast_util.h"
#include"for_each_expr.h"
#include"well_sorted.h"

macro_util::macro_util(ast_manager & m, simplifier & s):
    m_manager(m),
    m_simplifier(s),
    m_arith_simp(0),
    m_bv_simp(0),
    m_basic_simp(0), 
    m_forbidden_set(0),
    m_curr_clause(0) {
}

arith_simplifier_plugin * macro_util::get_arith_simp() const {
    if (m_arith_simp == 0) {
        const_cast<macro_util*>(this)->m_arith_simp = static_cast<arith_simplifier_plugin*>(m_simplifier.get_plugin(m_manager.get_family_id("arith")));
    }
    SASSERT(m_arith_simp != 0);
    return m_arith_simp;
}

bv_simplifier_plugin * macro_util::get_bv_simp() const {
    if (m_bv_simp == 0) {
        const_cast<macro_util*>(this)->m_bv_simp = static_cast<bv_simplifier_plugin*>(m_simplifier.get_plugin(m_manager.get_family_id("bv")));
    }
    SASSERT(m_bv_simp != 0);
    return m_bv_simp;
}

basic_simplifier_plugin * macro_util::get_basic_simp() const {
    if (m_basic_simp == 0) {
        const_cast<macro_util*>(this)->m_basic_simp = static_cast<basic_simplifier_plugin*>(m_simplifier.get_plugin(m_manager.get_basic_family_id()));
    }
    SASSERT(m_basic_simp != 0);
    return m_basic_simp;
}

bool macro_util::is_bv(expr * n) const {
    return get_bv_simp()->is_bv(n); 
}

bool macro_util::is_bv_sort(sort * s) const {
    return get_bv_simp()->is_bv_sort(s); 
}

bool macro_util::is_add(expr * n) const {
    return get_arith_simp()->is_add(n) || get_bv_simp()->is_add(n); 
}

bool macro_util::is_times_minus_one(expr * n, expr * & arg) const {
    return get_arith_simp()->is_times_minus_one(n, arg) || get_bv_simp()->is_times_minus_one(n, arg);
}

bool macro_util::is_le(expr * n) const { 
    return get_arith_simp()->is_le(n) || get_bv_simp()->is_le(n); 
}

bool macro_util::is_le_ge(expr * n) const {
    return get_arith_simp()->is_le_ge(n) || get_bv_simp()->is_le_ge(n);
}

poly_simplifier_plugin * macro_util::get_poly_simp_for(sort * s) const {
    if (is_bv_sort(s))
        return get_bv_simp();
    else
        return get_arith_simp();
}

app * macro_util::mk_zero(sort * s) const {
    poly_simplifier_plugin * ps = get_poly_simp_for(s);
    ps->set_curr_sort(s);
    return ps->mk_zero();
}

void macro_util::mk_sub(expr * t1, expr * t2, expr_ref & r) const {
    if (is_bv(t1)) {
        get_bv_simp()->mk_sub(t1, t2, r);
    }
    else {
        get_arith_simp()->mk_sub(t1, t2, r);
    }
}

void macro_util::mk_add(expr * t1, expr * t2, expr_ref & r) const {
    if (is_bv(t1)) {
        get_bv_simp()->mk_add(t1, t2, r);
    }
    else {
        get_arith_simp()->mk_add(t1, t2, r);
    }
}

void macro_util::mk_add(unsigned num_args, expr * const * args, sort * s, expr_ref & r) const {
    if (num_args == 0) {
        r = mk_zero(s);
        return;
    }
    poly_simplifier_plugin * ps = get_poly_simp_for(s);
    ps->set_curr_sort(s);
    ps->mk_add(num_args, args, r);
}

/**
   \brief Return true if \c n is an application of the form
   
   (f x_{k_1}, ..., x_{k_n})

   where f is uninterpreted
   n == num_decls
   x_{k_i}'s are variables
   and {k_1, ..., k_n } is equals to the set {0, ..., num_decls-1}
*/
bool macro_util::is_macro_head(expr * n, unsigned num_decls) const {
    if (is_app(n) &&
        !to_app(n)->get_decl()->is_associative() &&
        to_app(n)->get_family_id() == null_family_id &&
        to_app(n)->get_num_args() == num_decls) {
        sbuffer<int> var2pos;
        var2pos.resize(num_decls, -1);
        for (unsigned i = 0; i < num_decls; i++) {
            expr * c = to_app(n)->get_arg(i);
            if (!is_var(c)) 
                return false;
            unsigned idx = to_var(c)->get_idx();
            if (idx >= num_decls || var2pos[idx] != -1)
                return false;
            var2pos[idx] = i;
        }
        return true;
    }
    return false;
}

/**
   \brief Return true if n is of the form
   
   (= (f x_{k_1}, ..., x_{k_n}) t)    OR
   (iff (f x_{k_1}, ..., x_{k_n}) t) 

   where
   
   is_macro_head((f x_{k_1}, ..., x_{k_n})) returns true   AND
   t does not contain f                                    AND
   f is not in forbidden_set

   In case of success
   head will contain (f x_{k_1}, ..., x_{k_n}) AND
   def  will contain t

*/
bool macro_util::is_left_simple_macro(expr * n, unsigned num_decls, app * & head, expr * & def) const {
    if (m_manager.is_eq(n) || m_manager.is_iff(n)) {
        expr * lhs = to_app(n)->get_arg(0);
        expr * rhs = to_app(n)->get_arg(1);
        if (is_macro_head(lhs, num_decls) && !is_forbidden(to_app(lhs)->get_decl()) && !occurs(to_app(lhs)->get_decl(), rhs)) {
            head = to_app(lhs);
            def  = rhs;
            return true;
        }
    }
    return false;
}

/**
   \brief Return true if n is of the form
   
   (= t (f x_{k_1}, ..., x_{k_n}))    OR
   (iff t (f x_{k_1}, ..., x_{k_n}))

   where
   
   is_macro_head((f x_{k_1}, ..., x_{k_n})) returns true   AND
   t does not contain f                                    AND
   f is not in forbidden_set

   In case of success
   head will contain (f x_{k_1}, ..., x_{k_n}) AND
   def  will contain t

*/
bool macro_util::is_right_simple_macro(expr * n, unsigned num_decls, app * & head, expr * & def) const {
    if (m_manager.is_eq(n) || m_manager.is_iff(n)) {
        expr * lhs = to_app(n)->get_arg(0);
        expr * rhs = to_app(n)->get_arg(1);
        if (is_macro_head(rhs, num_decls) && !is_forbidden(to_app(rhs)->get_decl()) && !occurs(to_app(rhs)->get_decl(), lhs)) {
            head = to_app(rhs);
            def  = lhs;
            return true;
        }
    }
    return false;
}

/**
   \brief Return true if n contains f. The method ignores the sub-expression \c exception.

   \remark n is a "polynomial".
*/
bool macro_util::poly_contains_head(expr * n, func_decl * f, expr * exception) const {
    expr * curr = n;
    unsigned num_args;
    expr * const * args;
    if (is_add(n)) {
        num_args = to_app(n)->get_num_args();
        args     = to_app(n)->get_args();
    }
    else {
        num_args = 1;
        args     = &n;
    }
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = args[i];
        if (arg != exception && occurs(f, arg))
            return true;
    }
    return false;
}

bool macro_util::is_arith_macro(expr * n, unsigned num_decls, app_ref & head, expr_ref & def, bool & inv) const {
    // TODO: obsolete... we should move to collect_arith_macro_candidates
    arith_simplifier_plugin * as = get_arith_simp();
    if (!m_manager.is_eq(n) && !as->is_le(n) && !as->is_ge(n))
        return false;
    expr * lhs = to_app(n)->get_arg(0);
    expr * rhs = to_app(n)->get_arg(1);

    if (!as->is_numeral(rhs))
        return false;
  
    inv = false;
    ptr_buffer<expr> args;
    expr * h = 0;
    unsigned lhs_num_args;
    expr * const * lhs_args;
    if (is_add(lhs)) {
        lhs_num_args = to_app(lhs)->get_num_args();
        lhs_args     = to_app(lhs)->get_args();
    }
    else {
        lhs_num_args = 1;
        lhs_args     = &lhs;
    }
    for (unsigned i = 0; i < lhs_num_args; i++) {
        expr * arg = lhs_args[i];
        expr * neg_arg;
        if (h == 0 && 
            is_macro_head(arg, num_decls) && 
            !is_forbidden(to_app(arg)->get_decl()) && 
            !poly_contains_head(lhs, to_app(arg)->get_decl(), arg)) {
            h = arg;
        }
        else if (h == 0 && as->is_times_minus_one(arg, neg_arg) &&
                 is_macro_head(neg_arg, num_decls) && 
                 !is_forbidden(to_app(neg_arg)->get_decl()) && 
                 !poly_contains_head(lhs, to_app(neg_arg)->get_decl(), arg)) {
            h = neg_arg;
            inv = true;
        }
        else {
            args.push_back(arg);
        }
    }
    if (h == 0)
        return false;
    head = to_app(h);
    expr_ref tmp(m_manager);
    as->mk_add(args.size(), args.c_ptr(), tmp);
    if (inv)
        as->mk_sub(tmp, rhs, def);
    else
        as->mk_sub(rhs, tmp, def);
    return true;
}

/**
   \brief Auxiliary function for is_pseudo_predicate_macro. It detects the pattern (= (f X) t)
*/
bool macro_util::is_pseudo_head(expr * n, unsigned num_decls, app * & head, app * & t) {
    if (!m_manager.is_eq(n)) 
        return false;
    expr * lhs = to_app(n)->get_arg(0);
    expr * rhs = to_app(n)->get_arg(1);
    if (!is_ground(lhs) && !is_ground(rhs))
        return false;
    sort * s = m_manager.get_sort(lhs);
    if (m_manager.is_uninterp(s))
        return false;
    sort_size sz = s->get_num_elements();
    if (sz.is_finite() && sz.size() == 1)
        return false;
    if (is_macro_head(lhs, num_decls)) {
        head = to_app(lhs);
        t    = to_app(rhs);
        return true;
    }
    if (is_macro_head(rhs, num_decls)) {
        head = to_app(rhs);
        t    = to_app(lhs);
        return true;
    }
    return false;
}

/**
   \brief Returns true if n if of the form (forall (X) (iff (= (f X) t) def[X]))
   where t is a ground term, (f X) is the head. 
*/
bool macro_util::is_pseudo_predicate_macro(expr * n, app * & head, app * & t, expr * & def) {
    if (!is_quantifier(n) || !to_quantifier(n)->is_forall())
        return false;
    TRACE("macro_util", tout << "processing: " << mk_pp(n, m_manager) << "\n";);
    expr * body        = to_quantifier(n)->get_expr();
    unsigned num_decls = to_quantifier(n)->get_num_decls();
    if (!m_manager.is_iff(body))
        return false;
    expr * lhs = to_app(body)->get_arg(0);
    expr * rhs = to_app(body)->get_arg(1);
    if (is_pseudo_head(lhs, num_decls, head, t) && 
        !is_forbidden(head->get_decl()) && 
        !occurs(head->get_decl(), rhs)) {
        def = rhs;
        return true;
    }
    if (is_pseudo_head(rhs, num_decls, head, t) && 
        !is_forbidden(head->get_decl()) && 
        !occurs(head->get_decl(), lhs)) {
        def = lhs;
        return true;
    }
    return false;
}

/**
   \brief A quasi-macro head is of the form f[X_1, ..., X_n],
   where n == num_decls, f[X_1, ..., X_n] is a term starting with symbol f, f is uninterpreted,
   contains all universally quantified variables as arguments. 
   Note that, some arguments of f[X_1, ..., X_n] may not be variables.

   Examples of quasi-macros:
   f(x_1, x_1 + x_2, x_2) for num_decls == 2
   g(x_1, x_1)  for num_decls == 1

   Return true if \c n is a quasi-macro. Store the macro head in \c head, and the conditions to apply the macro in \c cond.
*/
bool macro_util::is_quasi_macro_head(expr * n, unsigned num_decls) const {
    if (is_app(n) &&
        to_app(n)->get_family_id() == null_family_id &&
        to_app(n)->get_num_args() >= num_decls) {
        unsigned num_args = to_app(n)->get_num_args();
        sbuffer<bool> found_vars;
        found_vars.resize(num_decls, false);
        unsigned num_found_vars = 0;
        for (unsigned i = 0; i < num_args; i++) {
            expr * arg = to_app(n)->get_arg(i);
            if (is_var(arg)) {
                unsigned idx = to_var(arg)->get_idx();
                if (idx >= num_decls)
                    return false;
                if (found_vars[idx] == false) {
                    found_vars[idx] = true;
                    num_found_vars++;
                }
            }
            else {
                if (occurs(to_app(n)->get_decl(), arg))
                    return false;
            }
        }
        return num_found_vars == num_decls;
    }
    return false;
}

/**
   \brief Convert a quasi-macro head into a macro head, and store the conditions under
   which it is valid in cond.
*/
void macro_util::quasi_macro_head_to_macro_head(app * qhead, unsigned num_decls, app_ref & head, expr_ref & cond) const {
    unsigned num_args = qhead->get_num_args();
    sbuffer<bool> found_vars;
    found_vars.resize(num_decls, false);
    ptr_buffer<expr> new_args;
    ptr_buffer<expr> new_conds;
    unsigned next_var_idx = num_decls;
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = qhead->get_arg(i);
        if (is_var(arg)) {
            unsigned idx = to_var(arg)->get_idx();
            SASSERT(idx < num_decls);
            if (found_vars[idx] == false) {
                found_vars[idx] = true;
                new_args.push_back(arg);
                continue;
            }
        }
        var * new_var   = m_manager.mk_var(next_var_idx, m_manager.get_sort(arg));
        next_var_idx++;
        expr * new_cond = m_manager.mk_eq(new_var, arg);
        new_args.push_back(new_var);
        new_conds.push_back(new_cond);
    }
    get_basic_simp()->mk_and(new_conds.size(), new_conds.c_ptr(), cond);
    head = m_manager.mk_app(qhead->get_decl(), new_args.size(), new_args.c_ptr());
}

/**
   \brief Given a macro defined by head and def, stores an interpretation for head->get_decl() in interp.
   This method assumes is_macro_head(head, head->get_num_args()) returns true,
   and def does not contain head->get_decl().

   See normalize_expr
*/
void macro_util::mk_macro_interpretation(app * head, expr * def, expr_ref & interp) const {
    SASSERT(is_macro_head(head, head->get_num_args()));
    SASSERT(!occurs(head->get_decl(), def));
    normalize_expr(head, def, interp);
}

/**
   \brief The variables in head may be in the wrong order.
   Example: f(x_1, x_0) instead of f(x_0, x_1)
   This method is essentially renaming the variables in t.
   Suppose t is g(x_1, x_0 + x_1)
   This method will store g(x_0, x_1 + x_0) in norm_t.

   f(x_1, x_2) --> f(x_0, x_1)
   f(x_3, x_2) --> f(x_0, x_1)
*/
void macro_util::normalize_expr(app * head, expr * t, expr_ref & norm_t) const {
    expr_ref_buffer  var_mapping(m_manager);
    bool changed = false;
    unsigned num_args = head->get_num_args();
    unsigned max      = num_args;
    for (unsigned i = 0; i < num_args; i++) {
        var * v     = to_var(head->get_arg(i));
        if (v->get_idx() >= max)
            max = v->get_idx() + 1;
    }
    TRACE("normalize_expr_bug",
          tout << "head: " << mk_pp(head, m_manager) << "\n";
          tout << "applying substitution to:\n" << mk_bounded_pp(t, m_manager) << "\n";);
    for (unsigned i = 0; i < num_args; i++) {
        var * v     = to_var(head->get_arg(i));
        if (v->get_idx() != i) {
            changed = true;
            var * new_var = m_manager.mk_var(i, v->get_sort());
            CTRACE("normalize_expr_bug", v->get_idx() >= num_args, tout << mk_pp(v, m_manager) << ", num_args: " << num_args << "\n";);
            SASSERT(v->get_idx() < max);
            var_mapping.setx(max - v->get_idx() - 1, new_var);
        }
        else {
            var_mapping.setx(max - i - 1, v);
        }
    }
    if (changed) {
        // REMARK: t may have nested quantifiers... So, I must use the std order for variable substitution.
        var_subst subst(m_manager); 
        TRACE("macro_util_bug",
              tout << "head: " << mk_pp(head, m_manager) << "\n";
              tout << "applying substitution to:\n" << mk_ll_pp(t, m_manager) << "\nsubstitituion:\n";
              for (unsigned i = 0; i < var_mapping.size(); i++) {
                  tout << "#" << i << " -> " << mk_pp(var_mapping[i], m_manager) << "\n";
              });
        subst(t, var_mapping.size(), var_mapping.c_ptr(), norm_t);
        SASSERT(is_well_sorted(m_manager, norm_t));
    }
    else {
        norm_t = t;
    }
}

// -----------------------------
//
// "Hint" support 
// See comment at is_hint_atom 
// for a definition of what a hint is.
//
// -----------------------------

bool is_hint_head(expr * n, ptr_buffer<var> & vars) {
    if (!is_app(n))
        return false;
    if (to_app(n)->get_decl()->is_associative() || to_app(n)->get_family_id() != null_family_id)
        return false;
    unsigned num_args = to_app(n)->get_num_args();
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = to_app(n)->get_arg(i); 
        if (is_var(arg))
            vars.push_back(to_var(arg));
    }
    return !vars.empty();
}

/**
   \brief Returns true if the variables in n is a subset of \c vars.
*/
bool vars_of_is_subset(expr * n, ptr_buffer<var> const & vars) {
    if (is_ground(n))
        return true;
    obj_hashtable<expr> visited;
    ptr_buffer<expr> todo;
    todo.push_back(n);
    while (!todo.empty()) {
        expr * curr = todo.back();
        todo.pop_back();
        if (is_var(curr)) {
            if (std::find(vars.begin(), vars.end(), to_var(curr)) == vars.end())
                return false;
        }
        else if (is_app(curr)) {
            unsigned num_args = to_app(curr)->get_num_args();
            for (unsigned i = 0; i < num_args; i++) {
                expr * arg = to_app(curr)->get_arg(i);
                if (is_ground(arg))
                    continue;
                if (visited.contains(arg))
                    continue;
                visited.insert(arg);
                todo.push_back(arg);
            }
        }
        else {
            SASSERT(is_quantifier(curr)); 
            return false; // do no support nested quantifier... being conservative.
        }
    }
    return true;
}

/**
   \brief (= lhs rhs) is a hint atom if 
   lhs is of the form (f t_1 ... t_n)
   and all variables occurring in rhs are direct arguments of lhs.
*/
bool is_hint_atom(expr * lhs, expr * rhs) {
    ptr_buffer<var> vars;
    if (!is_hint_head(lhs, vars))
        return false;
    return !occurs(to_app(lhs)->get_decl(), rhs) && vars_of_is_subset(rhs, vars);
}

void hint_to_macro_head(ast_manager & m, app * head, unsigned num_decls, app_ref & new_head) {
    unsigned num_args = head->get_num_args();
    ptr_buffer<expr> new_args;
    sbuffer<bool> found_vars;
    found_vars.resize(num_decls, false);
    unsigned next_var_idx = num_decls;
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = head->get_arg(i);
        if (is_var(arg)) {
            unsigned idx = to_var(arg)->get_idx();
            SASSERT(idx < num_decls);
            if (found_vars[idx] == false) {
                found_vars[idx] = true;
                new_args.push_back(arg);
                continue;
            }
        }
        var * new_var   = m.mk_var(next_var_idx, m.get_sort(arg));
        next_var_idx++;
        new_args.push_back(new_var);
    }
    new_head = m.mk_app(head->get_decl(), new_args.size(), new_args.c_ptr());
}

/**
   \brief Return true if n can be viewed as a polynomial "hint" based on head.
   That is, n (but the monomial exception) only uses the variables in head, and does not use
   head->get_decl(). 
   is_hint_head(head, vars) must also return true
*/
bool macro_util::is_poly_hint(expr * n, app * head, expr * exception) {
    TRACE("macro_util_hint", tout << "is_poly_hint n:\n" << mk_pp(n, m_manager) << "\nhead:\n" << mk_pp(head, m_manager) << "\nexception:\n"
          << mk_pp(exception, m_manager) << "\n";);
    ptr_buffer<var> vars;
    if (!is_hint_head(head, vars)) {
        TRACE("macro_util_hint", tout << "failed because head is not hint head\n";);
        return false;
    }
    func_decl * f = head->get_decl();
    unsigned num_args;
    expr * const * args;
    if (is_add(n)) {
        num_args = to_app(n)->get_num_args();
        args     = to_app(n)->get_args();
    }
    else {
        num_args = 1;
        args     = &n;
    }
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = args[i];
        if (arg != exception && (occurs(f, arg) || !vars_of_is_subset(arg, vars))) {
            TRACE("macro_util_hint", tout << "failed because of:\n" << mk_pp(arg, m_manager) << "\n";);
            return false;
        }
    }
    TRACE("macro_util_hint", tout << "succeeded\n";);
    return true;
    
}

// -----------------------------
//
// Macro candidates
//
// -----------------------------


macro_util::macro_candidates::macro_candidates(ast_manager & m):
    m_defs(m),
    m_conds(m) {
}

void macro_util::macro_candidates::reset() {
    m_fs.reset();
    m_defs.reset();
    m_conds.reset();
    m_ineq.reset();
    m_satisfy.reset();
    m_hint.reset();
}

void macro_util::macro_candidates::insert(func_decl * f, expr * def, expr * cond, bool ineq, bool satisfy_atom, bool hint) {
    m_fs.push_back(f);
    m_defs.push_back(def);
    m_conds.push_back(cond);
    m_ineq.push_back(ineq);
    m_satisfy.push_back(satisfy_atom);
    m_hint.push_back(hint);
}

// -----------------------------
//
// Macro util
//
// -----------------------------

void macro_util::insert_macro(app * head, expr * def, expr * cond, bool ineq, bool satisfy_atom, bool hint, macro_candidates & r) {
    expr_ref norm_def(m_manager);
    expr_ref norm_cond(m_manager);
    normalize_expr(head, def, norm_def);
    if (cond != 0)
        normalize_expr(head, cond, norm_cond);
    else if (!hint)
        norm_cond = m_manager.mk_true();
    SASSERT(!hint || norm_cond.get() == 0);
    r.insert(head->get_decl(), norm_def.get(), norm_cond.get(), ineq, satisfy_atom, hint);
}

void macro_util::insert_quasi_macro(app * head, unsigned num_decls, expr * def, expr * cond, bool ineq, bool satisfy_atom, 
                                    bool hint, macro_candidates & r) {
    if (!is_macro_head(head, head->get_num_args())) {
        app_ref  new_head(m_manager);
        expr_ref extra_cond(m_manager);
        expr_ref new_cond(m_manager);
        if (!hint) {
            quasi_macro_head_to_macro_head(head, num_decls, new_head, extra_cond);
            if (cond == 0)
                new_cond = extra_cond;
            else
                get_basic_simp()->mk_and(cond, extra_cond, new_cond);
        }
        else {
            hint_to_macro_head(m_manager, head, num_decls, new_head);
        }
        insert_macro(new_head, def, new_cond, ineq, satisfy_atom, hint, r);
    }
    else {
        insert_macro(head, def, cond, ineq, satisfy_atom, hint, r);
    }
}

bool macro_util::rest_contains_decl(func_decl * f, expr * except_lit) {
    if (m_curr_clause == 0)
        return false;
    SASSERT(is_clause(m_manager, m_curr_clause));
    unsigned num_lits = get_clause_num_literals(m_manager, m_curr_clause);
    for (unsigned i = 0; i < num_lits; i++) {
        expr * l = get_clause_literal(m_manager, m_curr_clause, i);
        if (l != except_lit && occurs(f, l))
            return true;
    }
    return false;
}

void macro_util::get_rest_clause_as_cond(expr * except_lit, expr_ref & extra_cond) {
    if (m_curr_clause == 0)
        return;
    SASSERT(is_clause(m_manager, m_curr_clause));
    basic_simplifier_plugin * bs = get_basic_simp();
    expr_ref_buffer neg_other_lits(m_manager);
    unsigned num_lits = get_clause_num_literals(m_manager, m_curr_clause);
    for (unsigned i = 0; i < num_lits; i++) {
        expr * l = get_clause_literal(m_manager, m_curr_clause, i);
        if (l != except_lit) {
            expr_ref neg_l(m_manager);
            bs->mk_not(l, neg_l);
            neg_other_lits.push_back(neg_l);
        }
    }
    if (neg_other_lits.empty())
        return;
    get_basic_simp()->mk_and(neg_other_lits.size(), neg_other_lits.c_ptr(), extra_cond);
}

void macro_util::collect_poly_args(expr * n, expr * exception, ptr_buffer<expr> & args) {
    args.reset();
    bool stop = false;
    unsigned num_args;
    expr * const * _args;
    if (is_add(n)) {
        num_args = to_app(n)->get_num_args();
        _args     = to_app(n)->get_args();
    }
    else {
        num_args = 1;
        _args     = &n;
    }
    for (unsigned i = 0; i < num_args; i++) {
        expr * arg = _args[i];
        if (arg != exception)
            args.push_back(arg);
    }
}

void macro_util::add_arith_macro_candidate(app * head, unsigned num_decls, expr * def, expr * atom, bool ineq, bool hint, macro_candidates & r) {
    expr_ref cond(m_manager);
    if (!hint)
        get_rest_clause_as_cond(atom, cond);
    insert_quasi_macro(head, num_decls, def, cond, ineq, true, hint, r);
}

void macro_util::collect_arith_macro_candidates(expr * lhs, expr * rhs, expr * atom, unsigned num_decls, bool is_ineq, macro_candidates & r) {
    if (!is_add(lhs) && m_manager.is_eq(atom)) // this case is a simple macro.
        return;
    bool stop = false;
    ptr_buffer<expr> args;
    unsigned lhs_num_args;
    expr * const * lhs_args;
    if (is_add(lhs)) {
        lhs_num_args = to_app(lhs)->get_num_args();
        lhs_args     = to_app(lhs)->get_args();
    }
    else {
        lhs_num_args = 1;
        lhs_args     = &lhs;
    }
    for (unsigned i = 0; i < lhs_num_args; i++) {
        expr * arg = lhs_args[i];
        expr * neg_arg;
        if (!is_app(arg))
            continue;
        func_decl * f = to_app(arg)->get_decl();
        
        bool _is_arith_macro = 
            is_quasi_macro_head(arg, num_decls) &&
            !is_forbidden(f) &&
            !poly_contains_head(lhs, f, arg) &&
            !occurs(f, rhs) &&
            !rest_contains_decl(f, atom);
        bool _is_poly_hint   = !_is_arith_macro && is_poly_hint(lhs, to_app(arg), arg);

        if (_is_arith_macro || _is_poly_hint) {
            collect_poly_args(lhs, arg, args);
            expr_ref rest(m_manager);
            mk_add(args.size(), args.c_ptr(), m_manager.get_sort(arg), rest);
            expr_ref def(m_manager);
            mk_sub(rhs, rest, def);
            add_arith_macro_candidate(to_app(arg), num_decls, def, atom, is_ineq, _is_poly_hint, r);
        }
        else if (is_times_minus_one(arg, neg_arg) && is_app(neg_arg)) {
            f = to_app(neg_arg)->get_decl();
            bool _is_arith_macro = 
                is_quasi_macro_head(neg_arg, num_decls) &&
                !is_forbidden(f) &&
                !poly_contains_head(lhs, f, arg) &&
                !occurs(f, rhs) &&
                !rest_contains_decl(f, atom);
            bool _is_poly_hint   = !_is_arith_macro && is_poly_hint(lhs, to_app(neg_arg), arg);
            
            if (_is_arith_macro || _is_poly_hint) {
                collect_poly_args(lhs, arg, args);
                expr_ref rest(m_manager);
                mk_add(args.size(), args.c_ptr(), m_manager.get_sort(arg), rest);
                expr_ref def(m_manager);
                mk_sub(rest, rhs, def);
                add_arith_macro_candidate(to_app(neg_arg), num_decls, def, atom, is_ineq, _is_poly_hint, r);
            }
        }
    }
}

void macro_util::collect_arith_macro_candidates(expr * atom, unsigned num_decls, macro_candidates & r) {
    TRACE("macro_util_hint", tout << "collect_arith_macro_candidates:\n" << mk_pp(atom, m_manager) << "\n";);
    if (!m_manager.is_eq(atom) && !is_le_ge(atom))
        return;
    expr * lhs = to_app(atom)->get_arg(0);
    expr * rhs = to_app(atom)->get_arg(1);
    bool is_ineq = !m_manager.is_eq(atom);
    collect_arith_macro_candidates(lhs, rhs, atom, num_decls, is_ineq, r);
    collect_arith_macro_candidates(rhs, lhs, atom, num_decls, is_ineq, r);
}

/**
   \brief Collect macro candidates for atom \c atom.
   The candidates are stored in \c r.
   
   The following post-condition holds:

   for each i in [0, r.size() - 1]
      we have a conditional macro of the form
      
      r.get_cond(i) IMPLIES f(x_1, ..., x_n) = r.get_def(i)
      
      where
         f == r.get_fs(i) .., x_n), f is uninterpreted and x_1, ..., x_n are variables.
         r.get_cond(i) and r.get_defs(i) do not contain f or variables not in {x_1, ..., x_n}

      The idea is to use r.get_defs(i) as the interpretation for f in a model M whenever r.get_cond(i)
   
   Given a model M and values { v_1, ..., v_n } 
   Let M' be M{x_1 -> v_1, ..., v_n -> v_n}
   
      Note that M'(f(x_1, ..., x_n)) = M(f)(v_1, ..., v_n)
      
      Then, IF we have that M(f)(v_1, ..., v_n) = M'(r.get_def(i)) AND
                            M'(r.get_cond(i))   = true
            THEN M'(atom) = true

      That is, if the conditional macro is used then the atom is satisfied when  M'(r.get_cond(i)) = true
      
      IF r.is_ineq(i) = false, then
      M(f)(v_1, ..., v_n) ***MUST BE*** M'(r.get_def(i)) whenever M'(r.get_cond(i)) = true
 
      IF r.satisfy_atom(i) = true, then we have the stronger property:

      Then, IF we have that (M'(r.get_cond(i)) = true IMPLIES M(f)(v_1, ..., v_n) = M'(r.get_def(i)))
            THEN M'(atom) = true
*/
void macro_util::collect_macro_candidates_core(expr * atom, unsigned num_decls, macro_candidates & r) {
    if (m_manager.is_eq(atom) || m_manager.is_iff(atom)) {
        expr * lhs = to_app(atom)->get_arg(0);
        expr * rhs = to_app(atom)->get_arg(1);
        if (is_quasi_macro_head(lhs, num_decls) && 
            !is_forbidden(to_app(lhs)->get_decl()) && 
            !occurs(to_app(lhs)->get_decl(), rhs) &&
            !rest_contains_decl(to_app(lhs)->get_decl(), atom)) {
            expr_ref cond(m_manager);
            get_rest_clause_as_cond(atom, cond);
            insert_quasi_macro(to_app(lhs), num_decls, rhs, cond, false, true, false, r);
        }
        else if (is_hint_atom(lhs, rhs)) {
            insert_quasi_macro(to_app(lhs), num_decls, rhs, 0, false, true, true, r);
        }

        
        if (is_quasi_macro_head(rhs, num_decls) && 
            !is_forbidden(to_app(rhs)->get_decl()) && 
            !occurs(to_app(rhs)->get_decl(), lhs) &&
            !rest_contains_decl(to_app(rhs)->get_decl(), atom)) {
            expr_ref cond(m_manager);
            get_rest_clause_as_cond(atom, cond);
            insert_quasi_macro(to_app(rhs), num_decls, lhs, cond, false, true, false, r);
        }
        else if (is_hint_atom(rhs, lhs)) {
            insert_quasi_macro(to_app(rhs), num_decls, lhs, 0, false, true, true, r);
        }
    }

    collect_arith_macro_candidates(atom, num_decls, r);
}

void macro_util::collect_macro_candidates(expr * atom, unsigned num_decls, macro_candidates & r) {
    m_curr_clause = 0;
    r.reset();
    collect_macro_candidates_core(atom, num_decls, r);
}

void macro_util::collect_macro_candidates(quantifier * q, macro_candidates & r) {
    r.reset();
    expr * n = q->get_expr();
    if (has_quantifiers(n))
        return;
    unsigned num_decls = q->get_num_decls();
    SASSERT(m_curr_clause == 0);
    if (is_clause(m_manager, n)) {
        m_curr_clause = n;
        unsigned num_lits = get_clause_num_literals(m_manager, n);
        for (unsigned i = 0; i < num_lits; i++)
            collect_macro_candidates_core(get_clause_literal(m_manager, n, i), num_decls, r);
        m_curr_clause = 0;
    }
    else {
        collect_macro_candidates_core(n, num_decls, r);
    }
}




/*++
Copyright (c) 2012 Microsoft Corporation

Module Name:

    qfauflia_tactic.h

Abstract:

    Tactic for QF_AUFLIA

Author:

    Leonardo (leonardo) 2012-02-21

Notes:

--*/
#ifndef _QFAUFLIA_TACTIC_H_
#define _QFAUFLIA_TACTIC_H_

#include"params.h"
class ast_manager;
class tactic;

tactic * mk_qfauflia_tactic(ast_manager & m, params_ref const & p = params_ref());

#endif

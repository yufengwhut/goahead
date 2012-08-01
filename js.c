/*
    js.c -- Mini JavaScript
  
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/

#include    "js.h"

//  MOB - refactor. if required, should be in goahead.h
#if CE
    #include    "CE/wincompat.h"
#endif

#if BIT_JAVASCRIPT
/********************************** Defines ***********************************/

#define     OCTAL   8
#define     HEX     16

static ej_t    **ejHandles;    /* List of js handles */
static int     ejMax = -1;     /* Maximum size of  */

/****************************** Forward Declarations **************************/

static ej_t     *ejPtr(int eid);
static void     clearString(char_t **ptr);
static void     setString(char_t **ptr, char_t *s);
static void     appendString(char_t **ptr, char_t *s);
static int      parse(ej_t *ep, int state, int flags);
static int      parseStmt(ej_t *ep, int state, int flags);
static int      parseDeclaration(ej_t *ep, int state, int flags);
static int      parseArgs(ej_t *ep, int state, int flags);
static int      parseCond(ej_t *ep, int state, int flags);
static int      parseExpr(ej_t *ep, int state, int flags);
static int      evalExpr(ej_t *ep, char_t *lhs, int rel, char_t *rhs);
static int      evalCond(ej_t *ep, char_t *lhs, int rel, char_t *rhs);
static int      evalFunction(ej_t *ep);
static void     freeFunc(ejfunc_t *func);
static void     ejRemoveNewlines(ej_t *ep, int state);

static int      getLexicalToken(ej_t* ep, int state);
static int      tokenAddChar(ej_t *ep, int c);
static int      inputGetc(ej_t* ep);
static void     inputPutback(ej_t* ep, int c);
static int      charConvert(ej_t* ep, int base, int maxDig);

/************************************* Code ***********************************/
/************************************* Code ***********************************/

int ejOpenEngine(sym_fd_t variables, sym_fd_t functions)
{
    ej_t    *ep;
    int     eid, vid;

    if ((eid = hAllocEntry((void***) &ejHandles, &ejMax, sizeof(ej_t))) < 0) {
        return -1;
    }
    ep = ejHandles[eid];
    ep->eid = eid;

    /*
        Create a top level symbol table if one is not provided for variables and functions. Variables may create other
        symbol tables for block level declarations so we use hAlloc to manage a list of variable tables.
     */
    if ((vid = hAlloc((void***) &ep->variables)) < 0) {
        ejMax = hFree((void***) &ejHandles, ep->eid);
        return -1;
    }
    if (vid >= ep->variableMax) {
        ep->variableMax = vid + 1;
    }

    if (variables == -1) {
        ep->variables[vid] = symOpen(64) + EJ_OFFSET;
        ep->flags |= FLAGS_VARIABLES;
    } else {
        ep->variables[vid] = variables + EJ_OFFSET;
    }

    if (functions == -1) {
        ep->functions = symOpen(64);
        ep->flags |= FLAGS_FUNCTIONS;
    } else {
        ep->functions = functions;
    }
    ejLexOpen(ep);

    /*
        Define standard constants
     */
    ejSetGlobalVar(ep->eid, T("null"), NULL);
    return ep->eid;
}


void ejCloseEngine(int eid)
{
    ej_t    *ep;
    int     i;

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }

    bfreeSafe(ep->error);
    ep->error = NULL;
    bfreeSafe(ep->result);
    ep->result = NULL;

    ejLexClose(ep);

    for (i = ep->variableMax - 1; i >= 0; i--) {
        if (ep->flags & FLAGS_VARIABLES) {
            symClose(ep->variables[i] - EJ_OFFSET);
        }
        ep->variableMax = hFree((void***) &ep->variables, i);
    }
    if (ep->flags & FLAGS_FUNCTIONS) {
        symClose(ep->functions);
    }
    ejMax = hFree((void***) &ejHandles, ep->eid);
    bfree(ep);
}


#if !ECOS && UNUSED
char_t *ejEvalFile(int eid, char_t *path, char_t **emsg)
{
    gstat_t sbuf;
    ej_t    *ep;
    char_t  *script, *rs;
    char    *fileBuf;
    int     fd;

    a_assert(path && *path);

    if (emsg) {
        *emsg = NULL;
    }

    if ((ep = ejPtr(eid)) == NULL) {
        return NULL;
    }
    //  MOB - refactor and push into gopen
#if !WIN
    fd = gopen(path, O_RDONLY | O_BINARY, 0666);
#else
    _sopen_s(&fd, path, O_RDONLY | O_BINARY, _SH_DENYNO, 0666);
#endif
    if (fd  < 0) {
        ejError(ep, T("Bad handle %d"), eid);
        return NULL;
    }
    if (gstat(path, &sbuf) < 0) {
        gclose(fd);
        ejError(ep, T("Cant stat %s"), path);
        return NULL;
    }
    if ((fileBuf = balloc(sbuf.st_size + 1)) == NULL) {
        gclose(fd);
        ejError(ep, T("Cant malloc %d"), sbuf.st_size);
        return NULL;
    }
    if (gread(fd, fileBuf, sbuf.st_size) != (int)sbuf.st_size) {
        gclose(fd);
        bfree(fileBuf);
        ejError(ep, T("Error reading %s"), path);
        return NULL;
    }
    fileBuf[sbuf.st_size] = '\0';
    gclose(fd);

    if ((script = ballocAscToUni(fileBuf, sbuf.st_size)) == NULL) {
        bfree(fileBuf);
        ejError(ep, T("Cant malloc %d"), sbuf.st_size + 1);
        return NULL;
    }
    bfree(fileBuf);
    rs = ejEvalBlock(eid, script, emsg);
    bfree(script);
    return rs;
}
#endif


/*
    Create a new variable scope block so that consecutive ejEval calls may be made with the same varible scope. This
    space MUST be closed with ejCloseBlock when the evaluations are complete.
 */
int ejOpenBlock(int eid)
{
    ej_t    *ep;
    int     vid;

    if((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    if ((vid = hAlloc((void***) &ep->variables)) < 0) {
        return -1;
    }
    if (vid >= ep->variableMax) {
        ep->variableMax = vid + 1;
    }
    ep->variables[vid] = symOpen(64) + EJ_OFFSET;
    return vid;

}


int ejCloseBlock(int eid, int vid)
{
    ej_t    *ep;

    if((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    symClose(ep->variables[vid] - EJ_OFFSET);
    ep->variableMax = hFree((void***) &ep->variables, vid);
    return 0;

}


/*
    Create a new variable scope block and evaluate a script. All variables
    created during this context will be automatically deleted when complete.
 */
char_t *ejEvalBlock(int eid, char_t *script, char_t **emsg)
{
    char_t* returnVal;
    int     vid;

    a_assert(script);

    vid = ejOpenBlock(eid);
    returnVal = ejEval(eid, script, emsg);
    ejCloseBlock(eid, vid);

    return returnVal;
}


/*
    Parse and evaluate a Ejscript. The caller may provide a symbol table to use for variables and function definitions.
    Return char_t pointer on success otherwise NULL pointer is returned.
 */
char_t *ejEval(int eid, char_t *script, char_t **emsg)
{
    ej_t    *ep;
    ejinput_t   *oldBlock;
    int     state;
    void    *endlessLoopTest;
    int     loopCounter;
    
    
    a_assert(script);

    if (emsg) {
        *emsg = NULL;
    } 
    if ((ep = ejPtr(eid)) == NULL) {
        return NULL;
    }
    setString(&ep->result, T(""));

    /*
        Allocate a new evaluation block, and save the old one
     */
    oldBlock = ep->input;
    ejLexOpenScript(ep, script);

    /*
        Do the actual parsing and evaluation
     */
    loopCounter = 0;
    endlessLoopTest = NULL;

    do {
        state = parse(ep, STATE_BEGIN, FLAGS_EXE);

        if (state == STATE_RET) {
            state = STATE_EOF;
        }
        /*
            prevent parser from going into infinite loop.  If parsing the same line 10 times then fail and report Syntax
            error.  Most normal error are caught in the parser itself.
         */
        if (endlessLoopTest == ep->input->script.servp) {
            if (loopCounter++ > 10) {
                state = STATE_ERR;
                ejError(ep, T("Syntax error"));
            }
        } else {
            endlessLoopTest = ep->input->script.servp;
            loopCounter = 0;
        }
    } while (state != STATE_EOF && state != STATE_ERR);

    ejLexCloseScript(ep);

    /*
        Return any error string to the user
     */
    if (state == STATE_ERR && emsg) {
        *emsg = bstrdup(ep->error);
    }

    /*
        Restore the old evaluation block
     */
    ep->input = oldBlock;

    if (state == STATE_EOF) {
        return ep->result;
    }
    if (state == STATE_ERR) {
        return NULL;
    }
    return ep->result;
}



/*
    Recursive descent parser for Ejscript
 */
static int parse(ej_t *ep, int state, int flags)
{
    a_assert(ep);

    switch (state) {
    /*
        Any statement, function arguments or conditional expressions
     */
    case STATE_STMT:
        if ((state = parseStmt(ep, state, flags)) != STATE_STMT_DONE &&
            state != STATE_EOF && state != STATE_STMT_BLOCK_DONE &&
            state != STATE_RET) {
            state = STATE_ERR;
        }
        break;

    case STATE_DEC:
        if ((state = parseStmt(ep, state, flags)) != STATE_DEC_DONE &&
            state != STATE_EOF) {
            state = STATE_ERR;
        }
        break;

    case STATE_EXPR:
        if ((state = parseStmt(ep, state, flags)) != STATE_EXPR_DONE &&
            state != STATE_EOF) {
            state = STATE_ERR;
        }
        break;

    /*
        Variable declaration list
     */
    case STATE_DEC_LIST:
        state = parseDeclaration(ep, state, flags);
        break;

    /*
        Function argument string
     */
    case STATE_ARG_LIST:
        state = parseArgs(ep, state, flags);
        break;

    /*
        Logical condition list (relational operations separated by &&, ||)
     */
    case STATE_COND:
        state = parseCond(ep, state, flags);
        break;

    /*
     j  Expression list
     */
    case STATE_RELEXP:
        state = parseExpr(ep, state, flags);
        break;
    }

    if (state == STATE_ERR && ep->error == NULL) {
        ejError(ep, T("Syntax error"));
    }
    return state;
}


/*
    Parse any statement including functions and simple relational operations
 */
static int parseStmt(ej_t *ep, int state, int flags)
{
    ejfunc_t    func;
    ejfunc_t    *saveFunc;
    ejinput_t   condScript, endScript, bodyScript, incrScript;
    char_t      *value, *identifier;
    int         done, expectSemi, thenFlags, elseFlags, tid, cond, forFlags;
    int         ejVarType;

    a_assert(ep);

    /*
        Set these to NULL, else we try to free them if an error occurs.
     */
    endScript.putBackToken = NULL;
    bodyScript.putBackToken = NULL;
    incrScript.putBackToken = NULL;
    condScript.putBackToken = NULL;

    expectSemi = 0;
    saveFunc = NULL;

    for (done = 0; !done; ) {
        tid = ejLexGetToken(ep, state);

        switch (tid) {
        default:
            ejLexPutbackToken(ep, TOK_EXPR, ep->token);
            done++;
            break;

        case TOK_ERR:
            state = STATE_ERR;
            done++;
            break;

        case TOK_EOF:
            state = STATE_EOF;
            done++;
            break;

        case TOK_NEWLINE:
            break;

        case TOK_SEMI:
            /*
                This case is when we discover no statement and just a lone ';'
             */
            if (state != STATE_STMT) {
                ejLexPutbackToken(ep, tid, ep->token);
            }
            done++;
            break;

        case TOK_ID:
            /*
                This could either be a reference to a variable or an assignment
             */
            identifier = NULL;
            setString(&identifier, ep->token);
            /*
                Peek ahead to see if this is an assignment
             */
            tid = ejLexGetToken(ep, state);
            if (tid == TOK_ASSIGNMENT) {
                if (parse(ep, STATE_RELEXP, flags) != STATE_RELEXP_DONE) {
                    clearString(&identifier);
                    goto error;
                }
                if (flags & FLAGS_EXE) {
                    if ( state == STATE_DEC ) {
                        ejSetLocalVar(ep->eid, identifier, ep->result);
                    } else {
                        ejVarType = ejGetVar(ep->eid, identifier, &value);
                        if (ejVarType > 0) {
                            ejSetLocalVar(ep->eid, identifier, ep->result);
                        } else {
                            ejSetGlobalVar(ep->eid, identifier, ep->result);
                        }
                    }
                }

            } else if (tid == TOK_INC_DEC ) {
                value = NULL;
                if (flags & FLAGS_EXE) {
                    ejVarType = ejGetVar(ep->eid, identifier, &value);
                    if (ejVarType < 0) {
                        ejError(ep, T("Undefined variable %s\n"), identifier);
                        goto error;
                    }
                    setString(&ep->result, value);
                    if (evalExpr(ep, value, (int) *ep->token, T("1")) < 0) {
                        state = STATE_ERR;
                        break;
                    }

                    if (ejVarType > 0) {
                        ejSetLocalVar(ep->eid, identifier, ep->result);
                    } else {
                        ejSetGlobalVar(ep->eid, identifier, ep->result);
                    }
                }

            } else {
                /*
                    If we are processing a declaration, allow undefined vars
                 */
                value = NULL;
                if (state == STATE_DEC) {
                    if (ejGetVar(ep->eid, identifier, &value) > 0) {
                        ejError(ep, T("Variable already declared"),
                            identifier);
                        clearString(&identifier);
                        goto error;
                    }
                    ejSetLocalVar(ep->eid, identifier, NULL);
                } else {
                    if ( flags & FLAGS_EXE ) {
                        if (ejGetVar(ep->eid, identifier, &value) < 0) {
                            ejError(ep, T("Undefined variable %s\n"),
                                identifier);
                            clearString(&identifier);
                            goto error;
                        }
                    }
                }
                setString(&ep->result, value);
                ejLexPutbackToken(ep, tid, ep->token);
            }
            clearString(&identifier);

            if (state == STATE_STMT) {
                expectSemi++;
            }
            done++;
            break;

        case TOK_LITERAL:
            /*
                Set the result to the literal (number or string constant)
             */
            setString(&ep->result, ep->token);
            if (state == STATE_STMT) {
                expectSemi++;
            }
            done++;
            break;

        case TOK_FUNCTION:
            /*
                We must save any current ep->func value for the current stack frame
             */
            if (ep->func) {
                saveFunc = ep->func;
            }
            memset(&func, 0, sizeof(ejfunc_t));
            setString(&func.fname, ep->token);
            ep->func = &func;

            setString(&ep->result, T(""));
            if (ejLexGetToken(ep, state) != TOK_LPAREN) {
                freeFunc(&func);
                goto error;
            }
            if (parse(ep, STATE_ARG_LIST, flags) != STATE_ARG_LIST_DONE) {
                freeFunc(&func);
                ep->func = saveFunc;
                goto error;
            }
            /*
                Evaluate the function if required
             */
            if (flags & FLAGS_EXE && evalFunction(ep) < 0) {
                freeFunc(&func);
                ep->func = saveFunc;
                goto error;
            }
            freeFunc(&func);
            ep->func = saveFunc;

            if (ejLexGetToken(ep, state) != TOK_RPAREN) {
                goto error;
            }
            if (state == STATE_STMT) {
                expectSemi++;
            }
            done++;
            break;

        case TOK_IF:
            if (state != STATE_STMT) {
                goto error;
            }
            if (ejLexGetToken(ep, state) != TOK_LPAREN) {
                goto error;
            }
            /*
                Evaluate the entire condition list "(condition)"
             */
            if (parse(ep, STATE_COND, flags) != STATE_COND_DONE) {
                goto error;
            }
            if (ejLexGetToken(ep, state) != TOK_RPAREN) {
                goto error;
            }
            /*
                This is the "then" case. We need to always parse both cases and execute only the relevant case.
             */
            if (*ep->result == '1') {
                thenFlags = flags;
                elseFlags = flags & ~FLAGS_EXE;
            } else {
                thenFlags = flags & ~FLAGS_EXE;
                elseFlags = flags;
            }
/*
 *          Process the "then" case.  Allow for RETURN statement
 */
            switch (parse(ep, STATE_STMT, thenFlags)) {
            case STATE_RET:
                return STATE_RET;
            case STATE_STMT_DONE:
                break;
            default:
                goto error;
            }
            /*
                check to see if there is an "else" case
             */
            ejRemoveNewlines(ep, state);
            tid = ejLexGetToken(ep, state);
            if (tid != TOK_ELSE) {
                ejLexPutbackToken(ep, tid, ep->token);
                done++;
                break;
            }
            /*
                Process the "else" case.  Allow for return.
             */
            switch (parse(ep, STATE_STMT, elseFlags)) {
            case STATE_RET:
                return STATE_RET;
            case STATE_STMT_DONE:
                break;
            default:
                goto error;
            }
            done++;
            break;

        case TOK_FOR:
            /*
                Format for the expression is:
                    for (initial; condition; incr) {
                        body;
                    }
             */
            if (state != STATE_STMT) {
                goto error;
            }
            if (ejLexGetToken(ep, state) != TOK_LPAREN) {
                goto error;
            }

            /*
                Evaluate the for loop initialization statement
             */
            if (parse(ep, STATE_EXPR, flags) != STATE_EXPR_DONE) {
                goto error;
            }
            if (ejLexGetToken(ep, state) != TOK_SEMI) {
                goto error;
            }

            /*
                The first time through, we save the current input context just to each step: prior to the conditional,
                the loop increment and the loop body.  
             */
            ejLexSaveInputState(ep, &condScript);
            if (parse(ep, STATE_COND, flags) != STATE_COND_DONE) {
                goto error;
            }
            cond = (*ep->result != '0');

            if (ejLexGetToken(ep, state) != TOK_SEMI) {
                goto error;
            }

            /*
                Don't execute the loop increment statement or the body first time
             */
            forFlags = flags & ~FLAGS_EXE;
            ejLexSaveInputState(ep, &incrScript);
            if (parse(ep, STATE_EXPR, forFlags) != STATE_EXPR_DONE) {
                goto error;
            }
            if (ejLexGetToken(ep, state) != TOK_RPAREN) {
                goto error;
            }

            /*
                Parse the body and remember the end of the body script
             */
            ejLexSaveInputState(ep, &bodyScript);
            if (parse(ep, STATE_STMT, forFlags) != STATE_STMT_DONE) {
                goto error;
            }
            ejLexSaveInputState(ep, &endScript);

            /*
                Now actually do the for loop. Note loop has been rotated
             */
            while (cond && (flags & FLAGS_EXE) ) {
                /*
                    Evaluate the body
                 */
                ejLexRestoreInputState(ep, &bodyScript);

                switch (parse(ep, STATE_STMT, flags)) {
                case STATE_RET:
                    return STATE_RET;
                case STATE_STMT_DONE:
                    break;
                default:
                    goto error;
                }
                /*
                    Evaluate the increment script
                 */
                ejLexRestoreInputState(ep, &incrScript);
                if (parse(ep, STATE_EXPR, flags) != STATE_EXPR_DONE) {
                    goto error;
                }
                /*
                    Evaluate the condition
                 */
                ejLexRestoreInputState(ep, &condScript);
                if (parse(ep, STATE_COND, flags) != STATE_COND_DONE) {
                    goto error;
                }
                cond = (*ep->result != '0');
            }
            ejLexRestoreInputState(ep, &endScript);
            done++;
            break;

        case TOK_VAR:
            if (parse(ep, STATE_DEC_LIST, flags) != STATE_DEC_LIST_DONE) {
                goto error;
            }
            done++;
            break;

        case TOK_COMMA:
            ejLexPutbackToken(ep, TOK_EXPR, ep->token);
            done++;
            break;

        case TOK_LPAREN:
            if (state == STATE_EXPR) {
                if (parse(ep, STATE_RELEXP, flags) != STATE_RELEXP_DONE) {
                    goto error;
                }
                if (ejLexGetToken(ep, state) != TOK_RPAREN) {
                    goto error;
                }
                return STATE_EXPR_DONE;
            }
            done++;
            break;

        case TOK_RPAREN:
            ejLexPutbackToken(ep, tid, ep->token);
            return STATE_EXPR_DONE;

        case TOK_LBRACE:
            /*
                This handles any code in braces except "if () {} else {}"
             */
            if (state != STATE_STMT) {
                goto error;
            }

            /*
                Parse will return STATE_STMT_BLOCK_DONE when the RBRACE is seen
             */
            do {
                state = parse(ep, STATE_STMT, flags);
            } while (state == STATE_STMT_DONE);

            /*
                Allow return statement.
             */
            if (state == STATE_RET) {
                return state;
            }

            if (ejLexGetToken(ep, state) != TOK_RBRACE) {
                goto error;
            }
            return STATE_STMT_DONE;

        case TOK_RBRACE:
            if (state == STATE_STMT) {
                ejLexPutbackToken(ep, tid, ep->token);
                return STATE_STMT_BLOCK_DONE;
            }
            goto error;

        case TOK_RETURN:
            if (parse(ep, STATE_RELEXP, flags) != STATE_RELEXP_DONE) {
                goto error;
            }
            if (flags & FLAGS_EXE) {
                while ( ejLexGetToken(ep, state) != TOK_EOF );
                done++;
                return STATE_RET;
            }
            break;
        }
    }

    if (expectSemi) {
        tid = ejLexGetToken(ep, state);
        if (tid != TOK_SEMI && tid != TOK_NEWLINE) {
            goto error;
        }
        /*
            Skip newline after semi-colon
         */
        ejRemoveNewlines(ep, state);
    }

doneParse:
    if (tid == TOK_FOR) {
        ejLexFreeInputState(ep, &condScript);
        ejLexFreeInputState(ep, &incrScript);
        ejLexFreeInputState(ep, &endScript);
        ejLexFreeInputState(ep, &bodyScript);
    }
    if (state == STATE_STMT) {
        return STATE_STMT_DONE;
    } else if (state == STATE_DEC) {
        return STATE_DEC_DONE;
    } else if (state == STATE_EXPR) {
        return STATE_EXPR_DONE;
    } else if (state == STATE_EOF) {
        return state;
    } else {
        return STATE_ERR;
    }

error:
    state = STATE_ERR;
    goto doneParse;
}


/*
    Parse variable declaration list
 */
static int parseDeclaration(ej_t *ep, int state, int flags)
{
    int     tid;

    a_assert(ep);

    /*
        Declarations can be of the following forms:
                var x;
                var x, y, z;
                var x = 1 + 2 / 3, y = 2 + 4;
      
        We set the variable to NULL if there is no associated assignment.
     */
    do {
        if ((tid = ejLexGetToken(ep, state)) != TOK_ID) {
            return STATE_ERR;
        }
        ejLexPutbackToken(ep, tid, ep->token);

        /*
            Parse the entire assignment or simple identifier declaration
         */
        if (parse(ep, STATE_DEC, flags) != STATE_DEC_DONE) {
            return STATE_ERR;
        }

        /*
            Peek at the next token, continue if comma seen
         */
        tid = ejLexGetToken(ep, state);
        if (tid == TOK_SEMI) {
            return STATE_DEC_LIST_DONE;
        } else if (tid != TOK_COMMA) {
            return STATE_ERR;
        }
    } while (tid == TOK_COMMA);

    if (tid != TOK_SEMI) {
        return STATE_ERR;
    }
    return STATE_DEC_LIST_DONE;
}


/*
    Parse function arguments
 */
static int parseArgs(ej_t *ep, int state, int flags)
{
    int     tid, aid;

    a_assert(ep);

    do {
        state = parse(ep, STATE_RELEXP, flags);
        if (state == STATE_EOF || state == STATE_ERR) {
            return state;
        }
        if (state == STATE_RELEXP_DONE) {
            aid = hAlloc((void***) &ep->func->args);
            ep->func->args[aid] = bstrdup(ep->result);
            ep->func->nArgs++;
        }
        /*
            Peek at the next token, continue if more args (ie. comma seen)
         */
        tid = ejLexGetToken(ep, state);
        if (tid != TOK_COMMA) {
            ejLexPutbackToken(ep, tid, ep->token);
        }
    } while (tid == TOK_COMMA);

    if (tid != TOK_RPAREN && state != STATE_RELEXP_DONE) {
        return STATE_ERR;
    }
    return STATE_ARG_LIST_DONE;
}


/*
    Parse conditional expression (relational ops separated by ||, &&)
 */
static int parseCond(ej_t *ep, int state, int flags)
{
    char_t  *lhs, *rhs;
    int     tid, operator;

    a_assert(ep);

    setString(&ep->result, T(""));
    rhs = lhs = NULL;
    operator = 0;

    do {
/*
 *  Recurse to handle one side of a conditional. Accumulate the
 *  left hand side and the final result in ep->result.
 */
        state = parse(ep, STATE_RELEXP, flags);
        if (state != STATE_RELEXP_DONE) {
            state = STATE_ERR;
            break;
        }

        if (operator > 0) {
            setString(&rhs, ep->result);
            if (evalCond(ep, lhs, operator, rhs) < 0) {
                state = STATE_ERR;
                break;
            }
        }
        setString(&lhs, ep->result);

        tid = ejLexGetToken(ep, state);
        if (tid == TOK_LOGICAL) {
            operator = (int) *ep->token;

        } else if (tid == TOK_RPAREN || tid == TOK_SEMI) {
            ejLexPutbackToken(ep, tid, ep->token);
            state = STATE_COND_DONE;
            break;

        } else {
            ejLexPutbackToken(ep, tid, ep->token);
        }

    } while (state == STATE_RELEXP_DONE);

    if (lhs) {
        bfree(lhs);
    }

    if (rhs) {
        bfree(rhs);
    }
    return state;
}


/*
    Parse expression (leftHandSide operator rightHandSide)
 */
static int parseExpr(ej_t *ep, int state, int flags)
{
    char_t  *lhs, *rhs;
    int     rel, tid;

    a_assert(ep);

    setString(&ep->result, T(""));
    rhs = lhs = NULL;
    rel = 0;
    tid = 0;

    do {
        /*
            This loop will handle an entire expression list. We call parse to evalutate each term which returns the
            result in ep->result.  
         */
        if (tid == TOK_LOGICAL) {
            if ((state = parse(ep, STATE_RELEXP, flags)) != STATE_RELEXP_DONE) {
                state = STATE_ERR;
                break;
            }
        } else {
            if ((state = parse(ep, STATE_EXPR, flags)) != STATE_EXPR_DONE) {
                state = STATE_ERR;
                break;
            }
        }

        if (rel > 0) {
            setString(&rhs, ep->result);
            if (tid == TOK_LOGICAL) {
                if (evalCond(ep, lhs, rel, rhs) < 0) {
                    state = STATE_ERR;
                    break;
                }
            } else {
                if (evalExpr(ep, lhs, rel, rhs) < 0) {
                    state = STATE_ERR;
                    break;
                }
            }
        }
        setString(&lhs, ep->result);

        if ((tid = ejLexGetToken(ep, state)) == TOK_EXPR ||
             tid == TOK_INC_DEC || tid == TOK_LOGICAL) {
            rel = (int) *ep->token;

        } else {
            ejLexPutbackToken(ep, tid, ep->token);
            state = STATE_RELEXP_DONE;
        }

    } while (state == STATE_EXPR_DONE);

    if (rhs) {
        bfree(rhs);
    }

    if (lhs) {
        bfree(lhs);
    }

    return state;
}


/*
    Evaluate a condition. Implements &&, ||, !
 */
static int evalCond(ej_t *ep, char_t *lhs, int rel, char_t *rhs)
{
    char_t  buf[16];
    int     l, r, lval;

    a_assert(lhs);
    a_assert(rhs);
    a_assert(rel > 0);

    lval = 0;
    if (gisdigit((int)*lhs) && gisdigit((int)*rhs)) {
        l = gatoi(lhs);
        r = gatoi(rhs);
        switch (rel) {
        case COND_AND:
            lval = l && r;
            break;
        case COND_OR:
            lval = l || r;
            break;
        default:
            ejError(ep, T("Bad operator %d"), rel);
            return -1;
        }
    } else {
        if (!gisdigit((int)*lhs)) {
            ejError(ep, T("Conditional must be numeric"), lhs);
        } else {
            ejError(ep, T("Conditional must be numeric"), rhs);
        }
    }

    stritoa(lval, buf, sizeof(buf));
    setString(&ep->result, buf);
    return 0;
}


/*
    Evaluate an operation
 */
static int evalExpr(ej_t *ep, char_t *lhs, int rel, char_t *rhs)
{
    char_t  *cp, buf[16];
    int     numeric, l, r, lval;

    a_assert(lhs);
    a_assert(rhs);
    a_assert(rel > 0);

    /*
        All of the characters in the lhs and rhs must be numeric
     */
    numeric = 1;
    for (cp = lhs; *cp; cp++) {
        if (!gisdigit((int)*cp)) {
            numeric = 0;
            break;
        }
    }

    if (numeric) {
        for (cp = rhs; *cp; cp++) {
            if (!gisdigit((int)*cp)) {
                numeric = 0;
                break;
            }
        }
    }

    if (numeric) {
        l = gatoi(lhs);
        r = gatoi(rhs);
        switch (rel) {
        case EXPR_PLUS:
            lval = l + r;
            break;
        case EXPR_INC:
            lval = l + 1;
            break;
        case EXPR_MINUS:
            lval = l - r;
            break;
        case EXPR_DEC:
            lval = l - 1;
            break;
        case EXPR_MUL:
            lval = l * r;
            break;
        case EXPR_DIV:
            if (r != 0) {
                lval = l / r;
            } else {
                lval = 0;
            }
            break;
        case EXPR_MOD:
            if (r != 0) {
                lval = l % r;
            } else {
                lval = 0;
            }
            break;
        case EXPR_LSHIFT:
            lval = l << r;
            break;
        case EXPR_RSHIFT:
            lval = l >> r;
            break;
        case EXPR_EQ:
            lval = l == r;
            break;
        case EXPR_NOTEQ:
            lval = l != r;
            break;
        case EXPR_LESS:
            lval = (l < r) ? 1 : 0;
            break;
        case EXPR_LESSEQ:
            lval = (l <= r) ? 1 : 0;
            break;
        case EXPR_GREATER:
            lval = (l > r) ? 1 : 0;
            break;
        case EXPR_GREATEREQ:
            lval = (l >= r) ? 1 : 0;
            break;
        case EXPR_BOOL_COMP:
            lval = (r == 0) ? 1 : 0;
            break;
        default:
            ejError(ep, T("Bad operator %d"), rel);
            return -1;
        }

    } else {
        switch (rel) {
        case EXPR_PLUS:
            clearString(&ep->result);
            appendString(&ep->result, lhs);
            appendString(&ep->result, rhs);
            return 0;
        case EXPR_LESS:
            lval = gstrcmp(lhs, rhs) < 0;
            break;
        case EXPR_LESSEQ:
            lval = gstrcmp(lhs, rhs) <= 0;
            break;
        case EXPR_GREATER:
            lval = gstrcmp(lhs, rhs) > 0;
            break;
        case EXPR_GREATEREQ:
            lval = gstrcmp(lhs, rhs) >= 0;
            break;
        case EXPR_EQ:
            lval = gstrcmp(lhs, rhs) == 0;
            break;
        case EXPR_NOTEQ:
            lval = gstrcmp(lhs, rhs) != 0;
            break;
        case EXPR_INC:
        case EXPR_DEC:
        case EXPR_MINUS:
        case EXPR_DIV:
        case EXPR_MOD:
        case EXPR_LSHIFT:
        case EXPR_RSHIFT:
        default:
            ejError(ep, T("Bad operator"));
            return -1;
        }
    }

    stritoa(lval, buf, sizeof(buf));
    setString(&ep->result, buf);
    return 0;
}


/*
    Evaluate a function
 */
static int evalFunction(ej_t *ep)
{
    sym_t   *sp;
    int     (*fn)(int eid, void *handle, int argc, char_t **argv);

    if ((sp = symLookup(ep->functions, ep->func->fname)) == NULL) {
        ejError(ep, T("Undefined procedure %s"), ep->func->fname);
        return -1;
    }
    fn = (int (*)(int, void*, int, char_t**)) sp->content.value.integer;
    if (fn == NULL) {
        ejError(ep, T("Undefined procedure %s"), ep->func->fname);
        return -1;
    }
    return (*fn)(ep->eid, ep->userHandle, ep->func->nArgs, ep->func->args);
}


/*
    Output a parse ej_error message
 */
void ejError(ej_t* ep, char_t* fmt, ...)
{
    va_list     args;
    ejinput_t   *ip;
    char_t      *errbuf, *msgbuf;

    a_assert(ep);
    a_assert(fmt);
    ip = ep->input;

    va_start(args, fmt);
    msgbuf = NULL;
    fmtValloc(&msgbuf, E_MAX_ERROR, fmt, args);
    va_end(args);

    if (ep && ip) {
        fmtAlloc(&errbuf, E_MAX_ERROR, T("%s\n At line %d, line => \n\n%s\n"), msgbuf, ip->lineNumber, ip->line);
        bfreeSafe(ep->error);
        ep->error = errbuf;
    }
    bfreeSafe(msgbuf);
}


static void clearString(char_t **ptr)
{
    a_assert(ptr);

    if (*ptr) {
        bfree(*ptr);
    }
    *ptr = NULL;
}


static void setString(char_t **ptr, char_t *s)
{
    a_assert(ptr);

    if (*ptr) {
        bfree(*ptr);
    }
    *ptr = bstrdup(s);
}


static void appendString(char_t **ptr, char_t *s)
{
    int len, oldlen, size;

    a_assert(ptr);

    if (*ptr) {
        len = gstrlen(s);
        oldlen = gstrlen(*ptr);
        size = (len + oldlen + 1) * sizeof(char_t);
        *ptr = brealloc(*ptr, size);
#if !WIN
        gstrcpy(&(*ptr)[oldlen], s);
#else
        strcpy_s(&(*ptr)[oldlen], size - oldlen, s);
#endif
    } else {
        *ptr = bstrdup(s);
    }
}


/*
    Define a function
 */
int ejSetGlobalFunction(int eid, char_t *name, int (*fn)(int eid, void *handle, int argc, char_t **argv))
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    return ejSetGlobalFunctionDirect(ep->functions, name, fn);
}


/*
    Define a function directly into the function symbol table.
 */
int ejSetGlobalFunctionDirect(sym_fd_t functions, char_t *name, int (*fn)(int eid, void *handle, int argc, char_t **argv))
{
    if (symEnter(functions, name, valueInteger((long) fn), 0) == NULL) {
        return -1;
    }
    return 0;
}


/*
    Remove ("undefine") a function
 */
int ejRemoveGlobalFunction(int eid, char_t *name)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    return symDelete(ep->functions, name);
}


void *ejGetGlobalFunction(int eid, char_t *name)
{
    ej_t    *ep;
    sym_t   *sp;
    int     (*fn)(int eid, void *handle, int argc, char_t **argv);

    if ((ep = ejPtr(eid)) == NULL) {
        return NULL;
    }

    if ((sp = symLookup(ep->functions, name)) != NULL) {
        fn = (int (*)(int, void*, int, char_t**)) sp->content.value.integer;
        return (void*) fn;
    }
    return NULL;
}


/*
    Utility routine to crack Ejscript arguments. Return the number of args
    seen. This routine only supports %s and %d type args.
  
    Typical usage:
  
        if (ejArgs(argc, argv, "%s %d", &name, &age) < 2) {
            error("Insufficient args\n");
            return -1;
        }
 */
int ejArgs(int argc, char_t **argv, char_t *fmt, ...)
{
    va_list vargs;
    char_t  *cp, **sp;
    int     *ip;
    int     argn;

    va_start(vargs, fmt);

    if (argv == NULL) {
        return 0;
    }
    for (argn = 0, cp = fmt; cp && *cp && argv[argn]; ) {
        if (*cp++ != '%') {
            continue;
        }
        switch (*cp) {
        case 'd':
            ip = va_arg(vargs, int*);
            *ip = gatoi(argv[argn]);
            break;

        case 's':
            sp = va_arg(vargs, char_t**);
            *sp = argv[argn];
            break;

        default:
            /*
                Unsupported
             */
            a_assert(0);
        }
        argn++;
    }

    va_end(vargs);
    return argn;
}


void ejSetUserHandle(int eid, void* handle)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }
    ep->userHandle = handle;
}


void* ejGetUserHandle(int eid)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return NULL;
    }
    return ep->userHandle;
}


int ejGetLineNumber(int eid)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    return ep->input->lineNumber;
}


void ejSetResult(int eid, char_t *s)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }
    setString(&ep->result, s);
}


char_t *ejGetResult(int eid)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return NULL;
    }
    return ep->result;
}

/*
    Set a variable. Note: a variable with a value of NULL means declared but undefined. The value is defined in the
    top-most variable frame.  
 */
void ejSetVar(int eid, char_t *var, char_t *value)
{
    ej_t    *ep;
    value_t v;

    a_assert(var && *var);

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }
    if (value == NULL) {
        v = valueString(value, 0);
    } else {
        v = valueString(value, VALUE_ALLOCATE);
    }
    symEnter(ep->variables[ep->variableMax - 1] - EJ_OFFSET, var, v, 0);
}


/*
    Set a local variable. Note: a variable with a value of NULL means declared but undefined. The value is defined in
    the top-most variable frame.  
 */
void ejSetLocalVar(int eid, char_t *var, char_t *value)
{
    ej_t    *ep;
    value_t v;

    a_assert(var && *var);

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }

    if (value == NULL) {
        v = valueString(value, 0);
    } else {
        v = valueString(value, VALUE_ALLOCATE);
    }
    symEnter(ep->variables[ep->variableMax - 1] - EJ_OFFSET, var, v, 0);
}


/*
    Set a global variable. Note: a variable with a value of NULL means declared but undefined. The value is defined in
    the global variable frame.  
 */
void ejSetGlobalVar(int eid, char_t *var, char_t *value)
{
    ej_t    *ep;
    value_t v;

    a_assert(var && *var);

    if ((ep = ejPtr(eid)) == NULL) {
        return;
    }
    if (value == NULL) {
        v = valueString(value, 0);
    } else {
        v = valueString(value, VALUE_ALLOCATE);
    }
    symEnter(ep->variables[0] - EJ_OFFSET, var, v, 0);
}


/*
    Get a variable
 */
int ejGetVar(int eid, char_t *var, char_t **value)
{
    ej_t    *ep;
    sym_t   *sp;
    int     i;

    a_assert(var && *var);
    a_assert(value);

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }

    i = ep->variableMax - 1;
    if ((sp = symLookup(ep->variables[i] - EJ_OFFSET, var)) == NULL) {
        i = 0;
        if ((sp = symLookup(ep->variables[0] - EJ_OFFSET, var)) == NULL) {
            return -1;
        }
    }
    a_assert(sp->content.type == string);
    *value = sp->content.value.string;
    return i;
}


sym_fd_t ejGetVariableTable(int eid)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    return *ep->variables;
}


sym_fd_t ejGetFunctionTable(int eid)
{
    ej_t    *ep;

    if ((ep = ejPtr(eid)) == NULL) {
        return -1;
    }
    return ep->functions;
}


/*
    Free an argument list
 */
static void freeFunc(ejfunc_t *func)
{
    int i;

    for (i = func->nArgs - 1; i >= 0; i--) {
        bfree(func->args[i]);
        func->nArgs = hFree((void***) &func->args, i);
    }

    if (func->fname) {
        bfree(func->fname);
        func->fname = NULL;
    }
}


/*
    Get Ejscript pointer
 */
static ej_t *ejPtr(int eid)
{
    a_assert(0 <= eid && eid < ejMax);

    if (eid < 0 || eid >= ejMax || ejHandles[eid] == NULL) {
        ejError(NULL, T("Bad handle %d"), eid);
        return NULL;
    }
    return ejHandles[eid];
}


/*
    This function removes any new lines.  Used for else cases, etc.
 */
static void ejRemoveNewlines(ej_t *ep, int state)
{
    int tid;

    do {
        tid = ejLexGetToken(ep, state);
    } while (tid == TOK_NEWLINE);
    ejLexPutbackToken(ep, tid, ep->token);
}


int ejLexOpen(ej_t* ep)
{
    return 0;
}


void ejLexClose(ej_t* ep)
{
}


int ejLexOpenScript(ej_t* ep, char_t *script)
{
    ejinput_t   *ip;

    a_assert(ep);
    a_assert(script);

    if ((ep->input = balloc(sizeof(ejinput_t))) == NULL) {
        return -1;
    }
    ip = ep->input;
    memset(ip, 0, sizeof(*ip));

    a_assert(ip);
    a_assert(ip->putBackToken == NULL);
    a_assert(ip->putBackTokenId == 0);

    /*
        Create the parse token buffer and script buffer
     */
    if (ringqOpen(&ip->tokbuf, EJ_INC, -1) < 0) {
        return -1;
    }
    if (ringqOpen(&ip->script, EJ_SCRIPT_INC, -1) < 0) {
        return -1;
    }
    /*
        Put the Ejscript into a ring queue for easy parsing
     */
    ringqPutStr(&ip->script, script);

    ip->lineNumber = 1;
    ip->lineLength = 0;
    ip->lineColumn = 0;
    ip->line = NULL;

    return 0;
}


void ejLexCloseScript(ej_t* ep)
{
    ejinput_t   *ip;

    a_assert(ep);

    ip = ep->input;
    a_assert(ip);

    if (ip->putBackToken) {
        bfree(ip->putBackToken);
        ip->putBackToken = NULL;
    }
    ip->putBackTokenId = 0;

    if (ip->line) {
        bfree(ip->line);
        ip->line = NULL;
    }
    ringqClose(&ip->tokbuf);
    ringqClose(&ip->script);
    bfree(ip);
}


void ejLexSaveInputState(ej_t* ep, ejinput_t* state)
{
    ejinput_t   *ip;

    a_assert(ep);

    ip = ep->input;
    a_assert(ip);

    *state = *ip;
    if (ip->putBackToken) {
        state->putBackToken = bstrdup(ip->putBackToken);
    }
}


void ejLexRestoreInputState(ej_t* ep, ejinput_t* state)
{
    ejinput_t   *ip;

    a_assert(ep);

    ip = ep->input;
    a_assert(ip);

    ip->tokbuf = state->tokbuf;
    ip->script = state->script;
    ip->putBackTokenId = state->putBackTokenId;
    if (ip->putBackToken) {
        bfree(ip->putBackToken);
    }
    if (state->putBackToken) {
        ip->putBackToken = bstrdup(state->putBackToken);
    }
}


void ejLexFreeInputState(ej_t* ep, ejinput_t* state)
{
    if (state->putBackToken) {
        bfree(state->putBackToken);
        state->putBackToken = NULL;
    }
}


int ejLexGetToken(ej_t* ep, int state)
{
    ep->tid = getLexicalToken(ep, state);
    return ep->tid;
}


static int getLexicalToken(ej_t* ep, int state)
{
    ringq_t     *inq, *tokq;
    ejinput_t*  ip;
    int         done, tid, c, quote, style;

    a_assert(ep);
    ip = ep->input;
    a_assert(ip);

    inq = &ip->script;
    tokq = &ip->tokbuf;
    ep->tid = -1;
    tid = -1;
    ep->token = T("");
    ringqFlush(tokq);

    if (ip->putBackTokenId > 0) {
        ringqPutStr(tokq, ip->putBackToken);
        tid = ip->putBackTokenId;
        ip->putBackTokenId = 0;
        ep->token = (char_t*) tokq->servp;
        return tid;
    }
    if ((c = inputGetc(ep)) < 0) {
        return TOK_EOF;
    }
    for (done = 0; !done; ) {
        switch (c) {
        case -1:
            return TOK_EOF;

        case ' ':
        case '\t':
        case '\r':
            do {
                if ((c = inputGetc(ep)) < 0)
                    break;
            } while (c == ' ' || c == '\t' || c == '\r');
            break;

        case '\n':
            return TOK_NEWLINE;

        case '(':
            tokenAddChar(ep, c);
            return TOK_LPAREN;

        case ')':
            tokenAddChar(ep, c);
            return TOK_RPAREN;

        case '{':
            tokenAddChar(ep, c);
            return TOK_LBRACE;

        case '}':
            tokenAddChar(ep, c);
            return TOK_RBRACE;

        case '+':
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c != '+' ) {
                inputPutback(ep, c);
                tokenAddChar(ep, EXPR_PLUS);
                return TOK_EXPR;
            }
            tokenAddChar(ep, EXPR_INC);
            return TOK_INC_DEC;

        case '-':
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c != '-' ) {
                inputPutback(ep, c);
                tokenAddChar(ep, EXPR_MINUS);
                return TOK_EXPR;
            }
            tokenAddChar(ep, EXPR_DEC);
            return TOK_INC_DEC;

        case '*':
            tokenAddChar(ep, EXPR_MUL);
            return TOK_EXPR;

        case '%':
            tokenAddChar(ep, EXPR_MOD);
            return TOK_EXPR;

        case '/':
            /*
                Handle the division operator and comments
             */
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c != '*' && c != '/') {
                inputPutback(ep, c);
                tokenAddChar(ep, EXPR_DIV);
                return TOK_EXPR;
            }
            style = c;
            /*
                Eat comments. Both C and C++ comment styles are supported.
             */
            while (1) {
                if ((c = inputGetc(ep)) < 0) {
                    ejError(ep, T("Syntax Error"));
                    return TOK_ERR;
                }
                if (c == '\n' && style == '/') {
                    break;
                } else if (c == '*') {
                    c = inputGetc(ep);
                    if (style == '/') {
                        if (c == '\n') {
                            break;
                        }
                    } else {
                        if (c == '/') {
                            break;
                        }
                    }
                }
            }
            /*
                Continue looking for a token, so get the next character
             */
            if ((c = inputGetc(ep)) < 0) {
                return TOK_EOF;
            }
            break;

        case '<':                                   /* < and <= */
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c == '<') {
                tokenAddChar(ep, EXPR_LSHIFT);
                return TOK_EXPR;
            } else if (c == '=') {
                tokenAddChar(ep, EXPR_LESSEQ);
                return TOK_EXPR;
            }
            tokenAddChar(ep, EXPR_LESS);
            inputPutback(ep, c);
            return TOK_EXPR;

        case '>':                                   /* > and >= */
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c == '>') {
                tokenAddChar(ep, EXPR_RSHIFT);
                return TOK_EXPR;
            } else if (c == '=') {
                tokenAddChar(ep, EXPR_GREATEREQ);
                return TOK_EXPR;
            }
            tokenAddChar(ep, EXPR_GREATER);
            inputPutback(ep, c);
            return TOK_EXPR;

        case '=':                                   /* "==" */
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c == '=') {
                tokenAddChar(ep, EXPR_EQ);
                return TOK_EXPR;
            }
            inputPutback(ep, c);
            return TOK_ASSIGNMENT;

        case '!':                                   /* "!=" or "!"*/
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            if (c == '=') {
                tokenAddChar(ep, EXPR_NOTEQ);
                return TOK_EXPR;
            }
            inputPutback(ep, c);
            tokenAddChar(ep, EXPR_BOOL_COMP);
            return TOK_EXPR;

        case ';':
            tokenAddChar(ep, c);
            return TOK_SEMI;

        case ',':
            tokenAddChar(ep, c);
            return TOK_COMMA;

        case '|':                                   /* "||" */
            if ((c = inputGetc(ep)) < 0 || c != '|') {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            tokenAddChar(ep, COND_OR);
            return TOK_LOGICAL;

        case '&':                                   /* "&&" */
            if ((c = inputGetc(ep)) < 0 || c != '&') {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }
            tokenAddChar(ep, COND_AND);
            return TOK_LOGICAL;

        case '\"':                                  /* String quote */
        case '\'':
            quote = c;
            if ((c = inputGetc(ep)) < 0) {
                ejError(ep, T("Syntax Error"));
                return TOK_ERR;
            }

            while (c != quote) {
                /*
                    check for escape sequence characters
                 */
                if (c == '\\') {
                    c = inputGetc(ep);

                    if (gisdigit(c)) {
                        /*
                            octal support, \101 maps to 65 = 'A'. put first char back so converter will work properly.
                         */
                        inputPutback(ep, c);
                        c = charConvert(ep, OCTAL, 3);

                    } else {
                        switch (c) {
                        case 'n':
                            c = '\n'; break;
                        case 'b':
                            c = '\b'; break;
                        case 'f':
                            c = '\f'; break;
                        case 'r':
                            c = '\r'; break;
                        case 't':
                            c = '\t'; break;
                        case 'x':
                            /*
                                hex support, \x41 maps to 65 = 'A'
                             */
                            c = charConvert(ep, HEX, 2);
                            break;
                        case 'u':
                            /*
                                unicode support, \x0401 maps to 65 = 'A'
                             */
                            c = charConvert(ep, HEX, 2);
                            c = c*16 + charConvert(ep, HEX, 2);

                            break;
                        case '\'':
                        case '\"':
                        case '\\':
                            break;
                        default:
                            ejError(ep, T("Invalid Escape Sequence"));
                            return TOK_ERR;
                        }
                    }
                    if (tokenAddChar(ep, c) < 0) {
                        return TOK_ERR;
                    }
                } else {
                    if (tokenAddChar(ep, c) < 0) {
                        return TOK_ERR;
                    }
                }
                if ((c = inputGetc(ep)) < 0) {
                    ejError(ep, T("Unmatched Quote"));
                    return TOK_ERR;
                }
            }
            return TOK_LITERAL;

        case '0': case '1': case '2': case '3': case '4': 
        case '5': case '6': case '7': case '8': case '9':
            do {
                if (tokenAddChar(ep, c) < 0) {
                    return TOK_ERR;
                }
                if ((c = inputGetc(ep)) < 0)
                    break;
            } while (gisdigit(c));
            inputPutback(ep, c);
            return TOK_LITERAL;

        default:
            /*
                Identifiers or a function names
             */
            while (1) {
                if (c == '\\') {
                    /*
                        just ignore any \ characters.
                     */
                } else if (tokenAddChar(ep, c) < 0) {
                        break;
                }
                if ((c = inputGetc(ep)) < 0) {
                    break;
                }
                if (!gisalnum(c) && c != '$' && c != '_' &&
                    c != '\\') {
                    break;
                }
            }
            if (! gisalpha(*tokq->servp) && *tokq->servp != '$' && 
                    *tokq->servp != '_') {
                ejError(ep, T("Invalid identifier %s"), tokq->servp);
                return TOK_ERR;
            }
            /*
                Check for reserved words (only "if", "else", "var", "for" and "return" at the moment)
             */
            if (state == STATE_STMT) {
                if (gstrcmp(ep->token, T("if")) == 0) {
                    return TOK_IF;
                } else if (gstrcmp(ep->token, T("else")) == 0) {
                    return TOK_ELSE;
                } else if (gstrcmp(ep->token, T("var")) == 0) {
                    return TOK_VAR;
                } else if (gstrcmp(ep->token, T("for")) == 0) {
                    return TOK_FOR;
                } else if (gstrcmp(ep->token, T("return")) == 0) {
                    if ((c == ';') || (c == '(')) {
                        inputPutback(ep, c);
                    }
                    return TOK_RETURN;
                }
            }
            /* 
                Skip white space after token to find out whether this is a function or not.
             */ 
            while (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if ((c = inputGetc(ep)) < 0)
                    break;
            }

            tid = (c == '(') ? TOK_FUNCTION : TOK_ID;
            done++;
        }
    }

    /*
        Putback the last extra character for next time
     */
    inputPutback(ep, c);
    return tid;
}


void ejLexPutbackToken(ej_t* ep, int tid, char_t *string)
{
    ejinput_t*  ip;

    a_assert(ep);
    ip = ep->input;
    a_assert(ip);

    if (ip->putBackToken) {
        bfree(ip->putBackToken);
    }
    ip->putBackTokenId = tid;
    ip->putBackToken = bstrdup(string);
}


static int tokenAddChar(ej_t *ep, int c)
{
    ejinput_t*  ip;

    a_assert(ep);
    ip = ep->input;
    a_assert(ip);

    if (ringqPutc(&ip->tokbuf, (char_t) c) < 0) {
        ejError(ep, T("Token too big"));
        return -1;
    }
    * ((char_t*) ip->tokbuf.endp) = '\0';
    ep->token = (char_t*) ip->tokbuf.servp;

    return 0;
}


static int inputGetc(ej_t* ep)
{
    ejinput_t   *ip;
    int         c, len;

    a_assert(ep);
    ip = ep->input;

    if ((len = ringqLen(&ip->script)) == 0) {
        return -1;
    }

    c = ringqGetc(&ip->script);

    if (c == '\n') {
        ip->lineNumber++;
        ip->lineColumn = 0;
    } else {
        if ((ip->lineColumn + 2) >= ip->lineLength) {
            ip->lineLength += EJ_INC;
            ip->line = brealloc(ip->line, ip->lineLength * sizeof(char_t));
        }
        ip->line[ip->lineColumn++] = c;
        ip->line[ip->lineColumn] = '\0';
    }
    return c;
}


static void inputPutback(ej_t* ep, int c)
{
    ejinput_t   *ip;

    a_assert(ep);

    ip = ep->input;
    ringqInsertc(&ip->script, (char_t) c);
    /* Fix by Fred Sauer, 2002/12/23 */
    if (ip->lineColumn > 0) {
        ip->lineColumn-- ;
    }
    ip->line[ip->lineColumn] = '\0';
}


/*
    Convert a hex or octal character back to binary, return original char if not a hex digit
 */
static int charConvert(ej_t* ep, int base, int maxDig)
{
    int     i, c, lval, convChar;

    lval = 0;
    for (i = 0; i < maxDig; i++) {
        if ((c = inputGetc(ep)) < 0) {
            break;
        }
        /*
            Initialize to out of range value
         */
        convChar = base;
        if (gisdigit(c)) {
            convChar = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            convChar = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            convChar = c - 'A' + 10;
        }
        /*
            if unexpected character then return it to buffer.
         */
        if (convChar >= base) {
            inputPutback(ep, c);
            break;
        }
        lval = (lval * base) + convChar;
    }
    return lval;
}

#endif /* BIT_JAVASCRIPT */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis GoAhead open source license or you may acquire 
    a commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
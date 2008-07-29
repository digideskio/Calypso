// Backend stubs

/* DMDFE backend stubs
 * This file contains the implementations of the backend routines.
 * For dmdfe these do nothing but print a message saying the module
 * has been parsed. Substitute your own behaviors for these routimes.
 */

#include <stdio.h>
#include <math.h>
#include <sstream>
#include <fstream>
#include <iostream>

#include "gen/llvm.h"

#include "attrib.h"
#include "total.h"
#include "init.h"
#include "mtype.h"
#include "template.h"
#include "hdrgen.h"
#include "port.h"

#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/tollvm.h"
#include "gen/llvmhelpers.h"
#include "gen/runtime.h"
#include "gen/arrays.h"
#include "gen/structs.h"
#include "gen/classes.h"
#include "gen/typeinf.h"
#include "gen/complex.h"
#include "gen/dvalue.h"
#include "gen/aa.h"
#include "gen/functions.h"
#include "gen/todebug.h"

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DeclarationExp::toElem(IRState* p)
{
    Logger::print("DeclarationExp::toElem: %s | T=%s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // variable declaration
    if (VarDeclaration* vd = declaration->isVarDeclaration())
    {
        Logger::println("VarDeclaration");

        // static
        if (vd->isDataseg())
        {
            vd->toObjFile(0); // TODO: multiobj
        }
        else
        {
            if (global.params.llvmAnnotate)
                DtoAnnotation(toChars());

            Logger::println("vdtype = %s", vd->type->toChars());

            // referenced by nested delegate?
            if (vd->nestedref) {
                Logger::println("has nestedref set");
                assert(vd->ir.irLocal);
                vd->ir.irLocal->value = p->func()->decl->ir.irFunc->nestedVar;
                assert(vd->ir.irLocal->value);
                assert(vd->ir.irLocal->nestedIndex >= 0);
            }
            // normal stack variable, allocate storage on the stack if it has not already been done
            else if(!vd->ir.irLocal) {
                const LLType* lltype = DtoType(vd->type);

                llvm::Value* allocainst;
                if(gTargetData->getTypeSizeInBits(lltype) == 0) 
                    allocainst = llvm::ConstantPointerNull::get(getPtrToType(lltype));
                else
                    allocainst = new llvm::AllocaInst(lltype, vd->toChars(), p->topallocapoint());

                //allocainst->setAlignment(vd->type->alignsize()); // TODO
                vd->ir.irLocal = new IrLocal(vd);
                vd->ir.irLocal->value = allocainst;

                if (global.params.symdebug)
                {
                    DtoDwarfLocalVariable(allocainst, vd);
                }
            }

            Logger::cout() << "llvm value for decl: " << *vd->ir.irLocal->value << '\n';
            DValue* ie = DtoInitializer(vd->init);
        }

        return new DVarValue(vd, vd->ir.getIrValue(), true);
    }
    // struct declaration
    else if (StructDeclaration* s = declaration->isStructDeclaration())
    {
        Logger::println("StructDeclaration");
        DtoForceConstInitDsymbol(s);
    }
    // function declaration
    else if (FuncDeclaration* f = declaration->isFuncDeclaration())
    {
        Logger::println("FuncDeclaration");
        DtoForceDeclareDsymbol(f);
    }
    // alias declaration
    else if (AliasDeclaration* a = declaration->isAliasDeclaration())
    {
        Logger::println("AliasDeclaration - no work");
        // do nothing
    }
    // enum
    else if (EnumDeclaration* e = declaration->isEnumDeclaration())
    {
        Logger::println("EnumDeclaration - no work");
        // do nothing
    }
    // class
    else if (ClassDeclaration* e = declaration->isClassDeclaration())
    {
        Logger::println("ClassDeclaration");
        DtoForceConstInitDsymbol(e);
    }
    // typedef
    else if (TypedefDeclaration* tdef = declaration->isTypedefDeclaration())
    {
        Logger::println("TypedefDeclaration");
        DtoTypeInfoOf(tdef->type, false);
    }
    // attribute declaration
    else if (AttribDeclaration* a = declaration->isAttribDeclaration())
    {
        Logger::println("AttribDeclaration");
        for (int i=0; i < a->decl->dim; ++i)
        {
            DtoForceDeclareDsymbol((Dsymbol*)a->decl->data[i]);
        }
    }
    // mixin declaration
    else if (TemplateMixin* m = declaration->isTemplateMixin())
    {
        Logger::println("TemplateMixin");
        for (int i=0; i < m->members->dim; ++i)
        {
            DtoForceDeclareDsymbol((Dsymbol*)m->members->data[i]);
        }
    }
    // unsupported declaration
    else
    {
        error("Unimplemented DeclarationExp type. kind: %s", declaration->kind());
        assert(0);
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* VarExp::toElem(IRState* p)
{
    Logger::print("VarExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    assert(var);
    if (VarDeclaration* vd = var->isVarDeclaration())
    {
        Logger::println("VarDeclaration %s", vd->toChars());

        // _arguments
        if (vd->ident == Id::_arguments)
        {
            Logger::println("Id::_arguments");
            LLValue* v = p->func()->_arguments;
            assert(v);
            return new DVarValue(vd, v, true);
        }
        // _argptr
        else if (vd->ident == Id::_argptr)
        {
            Logger::println("Id::_argptr");
            LLValue* v = p->func()->_argptr;
            assert(v);
            return new DVarValue(vd, v, true);
        }
        // _dollar
        else if (vd->ident == Id::dollar)
        {
            Logger::println("Id::dollar");
            assert(!p->arrays.empty());
            LLValue* tmp = DtoArrayLen(p->arrays.back());
            return new DVarValue(vd, tmp, false);
        }
        // typeinfo
        else if (TypeInfoDeclaration* tid = vd->isTypeInfoDeclaration())
        {
            Logger::println("TypeInfoDeclaration");
            DtoForceDeclareDsymbol(tid);
            assert(tid->ir.getIrValue());
            const LLType* vartype = DtoType(type);
            LLValue* m;
            if (tid->ir.getIrValue()->getType() != getPtrToType(vartype))
                m = p->ir->CreateBitCast(tid->ir.getIrValue(), vartype, "tmp");
            else
                m = tid->ir.getIrValue();
            return new DVarValue(vd, m, true);
        }
        // classinfo
        else if (ClassInfoDeclaration* cid = vd->isClassInfoDeclaration())
        {
            Logger::println("ClassInfoDeclaration: %s", cid->cd->toChars());
            DtoForceDeclareDsymbol(cid->cd);
            assert(cid->cd->ir.irStruct->classInfo);
            return new DVarValue(vd, cid->cd->ir.irStruct->classInfo, true);
        }
        // nested variable
        else if (vd->nestedref) {
            Logger::println("nested variable");
            return new DVarValue(vd, DtoNestedVariable(vd), true);
        }
        // function parameter
        else if (vd->isParameter()) {
            Logger::println("function param");
            FuncDeclaration* fd = vd->toParent2()->isFuncDeclaration();
            if (fd && fd != p->func()->decl) {
                Logger::println("nested parameter");
                return new DVarValue(vd, DtoNestedVariable(vd), true);
            }
            else if (vd->isRef() || vd->isOut() || DtoIsPassedByRef(vd->type) || llvm::isa<llvm::AllocaInst>(vd->ir.getIrValue())) {
                return new DVarValue(vd, vd->ir.getIrValue(), true);
            }
            else if (llvm::isa<llvm::Argument>(vd->ir.getIrValue())) {
                return new DImValue(type, vd->ir.getIrValue());
            }
            else assert(0);
        }
        else {
            // take care of forward references of global variables
            if (vd->isDataseg() || (vd->storage_class & STCextern)) {
                vd->toObjFile(0); // TODO: multiobj
                DtoConstInitGlobal(vd);
            }
            if (!vd->ir.getIrValue() || DtoType(vd->type)->isAbstract()) {
                error("global variable %s not resolved", vd->toChars());
                Logger::cout() << "unresolved global had type: " << *DtoType(vd->type) << '\n';
                fatal();
            }
            return new DVarValue(vd, vd->ir.getIrValue(), true);
        }
    }
    else if (FuncDeclaration* fdecl = var->isFuncDeclaration())
    {
        Logger::println("FuncDeclaration");
        LLValue* func = 0;
        if (fdecl->llvmInternal != LLVMva_arg) {
            DtoForceDeclareDsymbol(fdecl);
            func = fdecl->ir.irFunc->func;
        }
        return new DFuncValue(fdecl, func);
    }
    else if (SymbolDeclaration* sdecl = var->isSymbolDeclaration())
    {
        // this seems to be the static initialiser for structs
        Type* sdecltype = DtoDType(sdecl->type);
        Logger::print("Sym: type=%s\n", sdecltype->toChars());
        assert(sdecltype->ty == Tstruct);
        TypeStruct* ts = (TypeStruct*)sdecltype;
        assert(ts->sym);
        DtoForceConstInitDsymbol(ts->sym);
        assert(ts->sym->ir.irStruct->init);
        return new DVarValue(type, ts->sym->ir.irStruct->init, true);
    }
    else
    {
        assert(0 && "Unimplemented VarExp type");
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* VarExp::toConstElem(IRState* p)
{
    Logger::print("VarExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    if (SymbolDeclaration* sdecl = var->isSymbolDeclaration())
    {
        // this seems to be the static initialiser for structs
        Type* sdecltype = DtoDType(sdecl->type);
        Logger::print("Sym: type=%s\n", sdecltype->toChars());
        assert(sdecltype->ty == Tstruct);
        TypeStruct* ts = (TypeStruct*)sdecltype;
        DtoForceConstInitDsymbol(ts->sym);
        assert(ts->sym->ir.irStruct->constInit);
        return ts->sym->ir.irStruct->constInit;
    }
    else if (TypeInfoDeclaration* ti = var->isTypeInfoDeclaration())
    {
        const LLType* vartype = DtoType(type);
        LLConstant* m = DtoTypeInfoOf(ti->tinfo, false);
        if (m->getType() != getPtrToType(vartype))
            m = llvm::ConstantExpr::getBitCast(m, vartype);
        return m;
    }
    assert(0 && "Unsupported const VarExp kind");
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* IntegerExp::toElem(IRState* p)
{
    Logger::print("IntegerExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    LLConstant* c = toConstElem(p);
    return new DConstValue(type, c);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* IntegerExp::toConstElem(IRState* p)
{
    Logger::print("IntegerExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    const LLType* t = DtoType(type);
    if (isaPointer(t)) {
        Logger::println("pointer");
        LLConstant* i = llvm::ConstantInt::get(DtoSize_t(),(uint64_t)value,false);
        return llvm::ConstantExpr::getIntToPtr(i, t);
    }
    assert(llvm::isa<LLIntegerType>(t));
    LLConstant* c = llvm::ConstantInt::get(t,(uint64_t)value,!type->isunsigned());
    assert(c);
    Logger::cout() << "value = " << *c << '\n';
    return c;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* RealExp::toElem(IRState* p)
{
    Logger::print("RealExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    LLConstant* c = toConstElem(p);
    return new DConstValue(type, c);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* RealExp::toConstElem(IRState* p)
{
    Logger::print("RealExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    Type* t = DtoDType(type);
    return DtoConstFP(t, value);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* NullExp::toElem(IRState* p)
{
    Logger::print("NullExp::toElem(type=%s): %s\n", type->toChars(),toChars());
    LOG_SCOPE;
    LLConstant* c = toConstElem(p);
    return new DNullValue(type, c);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* NullExp::toConstElem(IRState* p)
{
    Logger::print("NullExp::toConstElem(type=%s): %s\n", type->toChars(),toChars());
    LOG_SCOPE;
    const LLType* t = DtoType(type);
    if (type->ty == Tarray) {
        assert(isaStruct(t));
        return llvm::ConstantAggregateZero::get(t);
    }
    else {
        return llvm::Constant::getNullValue(t);
    }
    assert(0);
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ComplexExp::toElem(IRState* p)
{
    Logger::print("ComplexExp::toElem(): %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    LLConstant* c = toConstElem(p);

    if (c->isNullValue()) {
        Type* t = DtoDType(type);
        if (t->ty == Tcomplex32)
            c = DtoConstFP(Type::tfloat32, 0);
        else
            c = DtoConstFP(Type::tfloat64, 0);
        return new DComplexValue(type, c, c);
    }

    return new DComplexValue(type, c->getOperand(0), c->getOperand(1));
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* ComplexExp::toConstElem(IRState* p)
{
    Logger::print("ComplexExp::toConstElem(): %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    return DtoConstComplex(type, value.re, value.im);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* StringExp::toElem(IRState* p)
{
    Logger::print("StringExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    Type* dtype = DtoDType(type);
    Type* cty = DtoDType(dtype->next);

    const LLType* ct = DtoTypeNotVoid(cty);
    //printf("ct = %s\n", type->next->toChars());
    const LLArrayType* at = LLArrayType::get(ct,len+1);

    LLConstant* _init;
    if (cty->size() == 1) {
        uint8_t* str = (uint8_t*)string;
        std::string cont((char*)str, len);
        _init = llvm::ConstantArray::get(cont,true);
    }
    else if (cty->size() == 2) {
        uint16_t* str = (uint16_t*)string;
        std::vector<LLConstant*> vals;
        for(size_t i=0; i<len; ++i) {
            vals.push_back(llvm::ConstantInt::get(ct, str[i], false));;
        }
        vals.push_back(llvm::ConstantInt::get(ct, 0, false));
        _init = llvm::ConstantArray::get(at,vals);
    }
    else if (cty->size() == 4) {
        uint32_t* str = (uint32_t*)string;
        std::vector<LLConstant*> vals;
        for(size_t i=0; i<len; ++i) {
            vals.push_back(llvm::ConstantInt::get(ct, str[i], false));;
        }
        vals.push_back(llvm::ConstantInt::get(ct, 0, false));
        _init = llvm::ConstantArray::get(at,vals);
    }
    else
    assert(0);

    llvm::GlobalValue::LinkageTypes _linkage = llvm::GlobalValue::InternalLinkage;//WeakLinkage;
    Logger::cout() << "type: " << *at << "\ninit: " << *_init << '\n';
    llvm::GlobalVariable* gvar = new llvm::GlobalVariable(at,true,_linkage,_init,".stringliteral",gIR->module);

    llvm::ConstantInt* zero = llvm::ConstantInt::get(LLType::Int32Ty, 0, false);
    LLConstant* idxs[2] = { zero, zero };
    LLConstant* arrptr = llvm::ConstantExpr::getGetElementPtr(gvar,idxs,2);

    if (dtype->ty == Tarray) {
        LLConstant* clen = llvm::ConstantInt::get(DtoSize_t(),len,false);
        LLValue* tmpmem = new llvm::AllocaInst(DtoType(dtype),"tempstring",p->topallocapoint());
        DtoSetArray(tmpmem, clen, arrptr);
        return new DVarValue(type, tmpmem, true);
    }
    else if (dtype->ty == Tsarray) {
        const LLType* dstType = getPtrToType(LLArrayType::get(ct, len));
        LLValue* emem = (gvar->getType() == dstType) ? gvar : DtoBitCast(gvar, dstType);
        return new DVarValue(type, emem, true);
    }
    else if (dtype->ty == Tpointer) {
        return new DImValue(type, arrptr);
    }

    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* StringExp::toConstElem(IRState* p)
{
    Logger::print("StringExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    Type* t = DtoDType(type);
    Type* cty = DtoDType(t->next);

    bool nullterm = (t->ty != Tsarray);
    size_t endlen = nullterm ? len+1 : len;

    const LLType* ct = DtoType(cty);
    const LLArrayType* at = LLArrayType::get(ct,endlen);

    LLConstant* _init;
    if (cty->size() == 1) {
        uint8_t* str = (uint8_t*)string;
        std::string cont((char*)str, len);
        _init = llvm::ConstantArray::get(cont, nullterm);
    }
    else if (cty->size() == 2) {
        uint16_t* str = (uint16_t*)string;
        std::vector<LLConstant*> vals;
        for(size_t i=0; i<len; ++i) {
            vals.push_back(llvm::ConstantInt::get(ct, str[i], false));;
        }
        if (nullterm)
            vals.push_back(llvm::ConstantInt::get(ct, 0, false));
        _init = llvm::ConstantArray::get(at,vals);
    }
    else if (cty->size() == 4) {
        uint32_t* str = (uint32_t*)string;
        std::vector<LLConstant*> vals;
        for(size_t i=0; i<len; ++i) {
            vals.push_back(llvm::ConstantInt::get(ct, str[i], false));;
        }
        if (nullterm)
            vals.push_back(llvm::ConstantInt::get(ct, 0, false));
        _init = llvm::ConstantArray::get(at,vals);
    }
    else
    assert(0);

    if (t->ty == Tsarray)
    {
        return _init;
    }

    llvm::GlobalValue::LinkageTypes _linkage = llvm::GlobalValue::InternalLinkage;//WeakLinkage;
    llvm::GlobalVariable* gvar = new llvm::GlobalVariable(_init->getType(),true,_linkage,_init,".stringliteral",gIR->module);

    llvm::ConstantInt* zero = llvm::ConstantInt::get(LLType::Int32Ty, 0, false);
    LLConstant* idxs[2] = { zero, zero };
    LLConstant* arrptr = llvm::ConstantExpr::getGetElementPtr(gvar,idxs,2);

    if (t->ty == Tpointer) {
        return arrptr;
    }
    else if (t->ty == Tarray) {
        LLConstant* clen = llvm::ConstantInt::get(DtoSize_t(),len,false);
        return DtoConstSlice(clen, arrptr);
    }

    assert(0);
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AssignExp::toElem(IRState* p)
{
    Logger::print("AssignExp::toElem: %s | %s = %s\n", toChars(), e1->type->toChars(), e2->type ? e2->type->toChars() : 0);
    LOG_SCOPE;

    if (e1->op == TOKarraylength)
    {
        Logger::println("performing array.length assignment");
        ArrayLengthExp *ale = (ArrayLengthExp *)e1;
        DValue* arr = ale->e1->toElem(p);
        DVarValue arrval(ale->e1->type, arr->getLVal(), true);
        DValue* newlen = e2->toElem(p);
        DSliceValue* slice = DtoResizeDynArray(arrval.getType(), &arrval, newlen);
        DtoAssign(loc, &arrval, slice);
        return newlen;
    }

    Logger::println("performing normal assignment");

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);
    DtoAssign(loc, l, r);

    if (l->isSlice() || l->isComplex())
        return l;

    LLValue* v;
    if (l->isVar() && l->isVar()->lval)
        v = l->getLVal();
    else
        v = l->getRVal();

    return new DVarValue(type, v, true);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AddExp::toElem(IRState* p)
{
    Logger::print("AddExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(type);
    Type* e1type = DtoDType(e1->type);
    Type* e1next = e1type->next ? DtoDType(e1type->next) : NULL;
    Type* e2type = DtoDType(e2->type);

    if (e1type != e2type) {
        if (llvmFieldIndex) {
            assert(e1type->ty == Tpointer && e1next && e1next->ty == Tstruct);
            Logger::println("add to AddrExp of struct");
            assert(r->isConst());
            llvm::ConstantInt* cofs = llvm::cast<llvm::ConstantInt>(r->isConst()->c);

            TypeStruct* ts = (TypeStruct*)e1next;
            DStructIndexVector offsets;
            LLValue* v = DtoIndexStruct(l->getRVal(), ts->sym, t->next, cofs->getZExtValue(), offsets);
            return new DFieldValue(type, v, true);
        }
        else if (e1type->ty == Tpointer) {
            Logger::println("add to pointer");
            if (r->isConst()) {
                llvm::ConstantInt* cofs = llvm::cast<llvm::ConstantInt>(r->isConst()->c);
                if (cofs->isZero()) {
                    Logger::println("is zero");
                    return new DImValue(type, l->getRVal());
                }
            }
            LLValue* v = llvm::GetElementPtrInst::Create(l->getRVal(), r->getRVal(), "tmp", p->scopebb());
            return new DImValue(type, v);
        }
        else if (t->iscomplex()) {
            return DtoComplexAdd(loc, type, l, r);
        }
        assert(0);
    }
    else if (t->iscomplex()) {
        return DtoComplexAdd(loc, type, l, r);
    }
    else {
        return DtoBinAdd(l,r);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AddAssignExp::toElem(IRState* p)
{
    Logger::print("AddAssignExp::toElem: %s\n", toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(type);

    DValue* res;
    if (DtoDType(e1->type)->ty == Tpointer) {
        LLValue* gep = llvm::GetElementPtrInst::Create(l->getRVal(),r->getRVal(),"tmp",p->scopebb());
        res = new DImValue(type, gep);
    }
    else if (t->iscomplex()) {
        res = DtoComplexAdd(loc, e1->type, l, r);
    }
    else {
        res = DtoBinAdd(l,r);
    }
    DtoAssign(loc, l, res);

    return res;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* MinExp::toElem(IRState* p)
{
    Logger::print("MinExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(type);
    Type* t1 = DtoDType(e1->type);
    Type* t2 = DtoDType(e2->type);

    if (t1->ty == Tpointer && t2->ty == Tpointer) {
        LLValue* lv = l->getRVal();
        LLValue* rv = r->getRVal();
        Logger::cout() << "lv: " << *lv << " rv: " << *rv << '\n';
        lv = p->ir->CreatePtrToInt(lv, DtoSize_t(), "tmp");
        rv = p->ir->CreatePtrToInt(rv, DtoSize_t(), "tmp");
        LLValue* diff = p->ir->CreateSub(lv,rv,"tmp");
        if (diff->getType() != DtoType(type))
            diff = p->ir->CreateIntToPtr(diff, DtoType(type), "tmp");
        return new DImValue(type, diff);
    }
    else if (t1->ty == Tpointer) {
        LLValue* idx = p->ir->CreateNeg(r->getRVal(), "tmp");
        LLValue* v = llvm::GetElementPtrInst::Create(l->getRVal(), idx, "tmp", p->scopebb());
        return new DImValue(type, v);
    }
    else if (t->iscomplex()) {
        return DtoComplexSub(loc, type, l, r);
    }
    else {
        return DtoBinSub(l,r);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* MinAssignExp::toElem(IRState* p)
{
    Logger::print("MinAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(type);

    DValue* res;
    if (DtoDType(e1->type)->ty == Tpointer) {
        Logger::println("ptr");
        LLValue* tmp = r->getRVal();
        LLValue* zero = llvm::ConstantInt::get(tmp->getType(),0,false);
        tmp = llvm::BinaryOperator::createSub(zero,tmp,"tmp",p->scopebb());
        tmp = llvm::GetElementPtrInst::Create(l->getRVal(),tmp,"tmp",p->scopebb());
        res = new DImValue(type, tmp);
    }
    else if (t->iscomplex()) {
        Logger::println("complex");
        res = DtoComplexSub(loc, type, l, r);
    }
    else {
        Logger::println("basic");
        res = DtoBinSub(l,r);
    }
    DtoAssign(loc, l, res);

    return res;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* MulExp::toElem(IRState* p)
{
    Logger::print("MulExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    if (type->iscomplex()) {
        return DtoComplexMul(loc, type, l, r);
    }

    return DtoBinMul(l,r);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* MulAssignExp::toElem(IRState* p)
{
    Logger::print("MulAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    DValue* res;
    if (type->iscomplex()) {
        res = DtoComplexMul(loc, type, l, r);
    }
    else {
        res = DtoBinMul(l,r);
    }
    DtoAssign(loc, l, res);

    return res;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DivExp::toElem(IRState* p)
{
    Logger::print("DivExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    if (type->iscomplex()) {
        return DtoComplexDiv(loc, type, l, r);
    }

    return DtoBinDiv(l, r);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DivAssignExp::toElem(IRState* p)
{
    Logger::print("DivAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    DValue* res;
    if (type->iscomplex()) {
        res = DtoComplexDiv(loc, type, l, r);
    }
    else {
        res = DtoBinDiv(l,r);
    }
    DtoAssign(loc, l, res);

    return res;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ModExp::toElem(IRState* p)
{
    Logger::print("ModExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    return DtoBinRem(l, r);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ModAssignExp::toElem(IRState* p)
{
    Logger::print("ModAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    DValue* res = DtoBinRem(l, r);
    DtoAssign(loc, l, res);

    return res;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CallExp::toElem(IRState* p)
{
    Logger::print("CallExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // get the callee value
    DValue* fnval = e1->toElem(p);

    // get func value if any
    DFuncValue* dfnval = fnval->isFunc();

    // handle magic intrinsics (mapping to instructions)
    bool va_intrinsic = false;
    if (dfnval && dfnval->func)
    {
        FuncDeclaration* fndecl = dfnval->func;
        // va_start instruction
        if (fndecl->llvmInternal == LLVMva_start) {
            // TODO
            assert(0 && "va_start not yet implemented");
        }
        // va_arg instruction
        else if (fndecl->llvmInternal == LLVMva_arg) {
            return DtoVaArg(loc, type, (Expression*)arguments->data[0]);
        }
        // C alloca
        else if (fndecl->llvmInternal == LLVMalloca) {
            Expression* exp = (Expression*)arguments->data[0];
            DValue* expv = exp->toElem(p);
            if (expv->getType()->toBasetype()->ty != Tint32)
                expv = DtoCast(loc, expv, Type::tint32);
            return new DImValue(type, gIR->ir->CreateAlloca(LLType::Int8Ty, expv->getRVal(), ".alloca"));
        }
    }

    return DtoCallFunction(loc, type, fnval, arguments);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CastExp::toElem(IRState* p)
{
    Logger::print("CastExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);
    DValue* v = DtoCast(loc, u, to);
    // force d type to this->type
    v->getType() = type;

    if (v->isSlice()) {
        // only valid as rvalue!
        return v;
    }

    else if(u->isLVal())
        return new DLRValue(u, v);

    else
        return v;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* SymOffExp::toElem(IRState* p)
{
    Logger::print("SymOffExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    assert(0 && "SymOffExp::toElem should no longer be called :/");
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AddrExp::toElem(IRState* p)
{
    Logger::println("AddrExp::toElem: %s | %s", toChars(), type->toChars());
    LOG_SCOPE;
    DValue* v = e1->toElem(p);
    if (v->isField()) {
        Logger::println("is field");
        return v;
    }
    else if (DFuncValue* fv = v->isFunc()) {
        Logger::println("is func");
        //Logger::println("FuncDeclaration");
        FuncDeclaration* fd = fv->func;
        assert(fd);
        DtoForceDeclareDsymbol(fd);
        return new DFuncValue(fd, fd->ir.irFunc->func);
    }
    else if (DImValue* im = v->isIm()) {
        Logger::println("is immediate");
        return v;
    }
    Logger::println("is nothing special");
    return new DFieldValue(type, v->getLVal(), false);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* PtrExp::toElem(IRState* p)
{
    Logger::println("PtrExp::toElem: %s | %s", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* a = e1->toElem(p);

    // this should be deterministic but right now lvalue casts don't propagate lvalueness !?!
    LLValue* lv = a->getRVal();
    LLValue* v = lv;
    if (DtoCanLoad(v))
        v = DtoLoad(v);
    return new DLRValue(new DVarValue(type, lv, true), new DImValue(type, v));
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DotVarExp::toElem(IRState* p)
{
    Logger::print("DotVarExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);

    Type* t = DtoDType(type);
    Type* e1type = DtoDType(e1->type);

    //Logger::println("e1type=%s", e1type->toChars());
    //Logger::cout() << *DtoType(e1type) << '\n';

    if (VarDeclaration* vd = var->isVarDeclaration()) {
        LLValue* arrptr;
        if (e1type->ty == Tpointer) {
            assert(e1type->next->ty == Tstruct);
            TypeStruct* ts = (TypeStruct*)e1type->next;
            Logger::println("Struct member offset:%d", vd->offset);

            LLValue* src = l->getRVal();

            DStructIndexVector vdoffsets;
            arrptr = DtoIndexStruct(src, ts->sym, vd->type, vd->offset, vdoffsets);
        }
        else if (e1type->ty == Tclass) {
            TypeClass* tc = (TypeClass*)e1type;
            Logger::println("Class member offset: %d", vd->offset);

            LLValue* src = l->getRVal();

            DStructIndexVector vdoffsets;
            arrptr = DtoIndexClass(src, tc->sym, vd->type, vd->offset, vdoffsets);
        }
        else
            assert(0);

        //Logger::cout() << "mem: " << *arrptr << '\n';
        return new DVarValue(vd, arrptr, true);
    }
    else if (FuncDeclaration* fdecl = var->isFuncDeclaration())
    {
        DtoResolveDsymbol(fdecl);

        LLValue* funcval;
        LLValue* vthis2 = 0;
        if (e1type->ty == Tclass) {
            TypeClass* tc = (TypeClass*)e1type;
            if (tc->sym->isInterfaceDeclaration()) {
                vthis2 = DtoCastInterfaceToObject(l, NULL)->getRVal();
            }
        }
        LLValue* vthis = l->getRVal();
        if (!vthis2) vthis2 = vthis;

        // super call
        if (e1->op == TOKsuper) {
            DtoForceDeclareDsymbol(fdecl);
            funcval = fdecl->ir.irFunc->func;
            assert(funcval);
        }
        // normal virtual call
        else if (fdecl->isAbstract() || (!fdecl->isFinal() && fdecl->isVirtual())) {
            assert(fdecl->vtblIndex > 0);
            assert(e1type->ty == Tclass);

            LLValue* zero = llvm::ConstantInt::get(LLType::Int32Ty, 0, false);
            LLValue* vtblidx = llvm::ConstantInt::get(LLType::Int32Ty, (size_t)fdecl->vtblIndex, false);
            //Logger::cout() << "vthis: " << *vthis << '\n';
            funcval = DtoGEP(vthis, zero, zero);
            funcval = DtoLoad(funcval);
            funcval = DtoGEP(funcval, zero, vtblidx, toChars());
            funcval = DtoLoad(funcval);
        #if OPAQUE_VTBLS
            funcval = DtoBitCast(funcval, getPtrToType(DtoType(fdecl->type)));
            Logger::cout() << "funcval casted: " << *funcval << '\n';
        #endif
        }
        // static call
        else {
            DtoForceDeclareDsymbol(fdecl);
            funcval = fdecl->ir.irFunc->func;
            assert(funcval);
        }
        return new DFuncValue(fdecl, funcval, vthis2);
    }
    else {
        printf("unsupported dotvarexp: %s\n", var->toChars());
    }

    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ThisExp::toElem(IRState* p)
{
    Logger::print("ThisExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // this seems to happen for dmd generated assert statements like:
    //      assert(this, "null this");
    if (!var)
    {
        LLValue* v = p->func()->thisVar;
        assert(v);
        return new DImValue(type, v);
    }
    // regular this expr
    else if (VarDeclaration* vd = var->isVarDeclaration()) {
        LLValue* v;
        v = p->func()->decl->ir.irFunc->thisVar;
        if (llvm::isa<llvm::AllocaInst>(v))
            v = DtoLoad(v);
        const LLType* t = DtoType(type);
        if (v->getType() != t)
            v = DtoBitCast(v, t);
        return new DThisValue(vd, v);
    }

    // anything we're not yet handling ?
    assert(0);
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* IndexExp::toElem(IRState* p)
{
    Logger::print("IndexExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);

    Type* e1type = DtoDType(e1->type);

    p->arrays.push_back(l); // if $ is used it must be an array so this is fine.
    DValue* r = e2->toElem(p);
    p->arrays.pop_back();

    LLValue* zero = DtoConstUint(0);
    LLValue* one = DtoConstUint(1);

    LLValue* arrptr = 0;
    if (e1type->ty == Tpointer) {
        arrptr = DtoGEP1(l->getRVal(),r->getRVal());
    }
    else if (e1type->ty == Tsarray) {
        arrptr = DtoGEP(l->getRVal(), zero, r->getRVal());
    }
    else if (e1type->ty == Tarray) {
        arrptr = DtoArrayPtr(l);
        arrptr = DtoGEP1(arrptr,r->getRVal());
    }
    else if (e1type->ty == Taarray) {
        return DtoAAIndex(loc, type, l, r);
    }
    else {
        Logger::println("invalid index exp! e1type: %s", e1type->toChars());
        assert(0);
    }
    return new DVarValue(type, arrptr, true);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* SliceExp::toElem(IRState* p)
{
    Logger::print("SliceExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // this is the new slicing code, it's different in that a full slice will no longer retain the original pointer.
    // but this was broken if there *was* no original pointer, ie. a slice of a slice...
    // now all slices have *both* the 'len' and 'ptr' fields set to != null.

    // value being sliced
    LLValue* elen;
    LLValue* eptr;
    DValue* e = e1->toElem(p);

    // handle pointer slicing
    Type* etype = e1->type->toBasetype();
    if (etype->ty == Tpointer)
    {
        assert(lwr);
        eptr = e->getRVal();
    }
    // array slice
    else
    {
        eptr = DtoArrayPtr(e);
    }

    // has lower bound, pointer needs adjustment
    if (lwr)
    {
        // must have upper bound too then
        assert(upr);

        // get bounds (make sure $ works)
        p->arrays.push_back(e);
        DValue* lo = lwr->toElem(p);
        DValue* up = upr->toElem(p);
        p->arrays.pop_back();
        LLValue* vlo = lo->getRVal();
        LLValue* vup = up->getRVal();

        // offset by lower
        eptr = DtoGEP1(eptr, vlo);

        // adjust length
        elen = p->ir->CreateSub(vup, vlo, "tmp");
    }
    // no bounds or full slice -> just convert to slice
    else
    {
        assert(e1->type->toBasetype()->ty != Tpointer);
        // if the sliceee is a static array, we use the length of that as DMD seems
        // to give contrary inconsistent sizesin some multidimensional static array cases.
        // (namely default initialization, int[16][16] arr; -> int[256] arr = 0;)
        if (etype->ty == Tsarray)
        {
            TypeSArray* tsa = (TypeSArray*)etype;
            elen = DtoConstSize_t(tsa->dim->toUInteger());
        }
        // for normal code the actual array length is what we want!
        else
        {
            elen = DtoArrayLen(e);
        }
    }

    return new DSliceValue(type, elen, eptr);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CmpExp::toElem(IRState* p)
{
    Logger::print("CmpExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(e1->type);
    Type* e2t = DtoDType(e2->type);
    assert(DtoType(t) == DtoType(e2t));

    LLValue* eval = 0;

    if (t->isintegral() || t->ty == Tpointer)
    {
        llvm::ICmpInst::Predicate cmpop;
        bool skip = false;
        switch(op)
        {
        case TOKlt:
        case TOKul:
            cmpop = t->isunsigned() ? llvm::ICmpInst::ICMP_ULT : llvm::ICmpInst::ICMP_SLT;
            break;
        case TOKle:
        case TOKule:
            cmpop = t->isunsigned() ? llvm::ICmpInst::ICMP_ULE : llvm::ICmpInst::ICMP_SLE;
            break;
        case TOKgt:
        case TOKug:
            cmpop = t->isunsigned() ? llvm::ICmpInst::ICMP_UGT : llvm::ICmpInst::ICMP_SGT;
            break;
        case TOKge:
        case TOKuge:
            cmpop = t->isunsigned() ? llvm::ICmpInst::ICMP_UGE : llvm::ICmpInst::ICMP_SGE;
            break;
        case TOKue:
            cmpop = llvm::ICmpInst::ICMP_EQ;
            break;
        case TOKlg:
            cmpop = llvm::ICmpInst::ICMP_NE;
            break;
        case TOKleg:
            skip = true;
            eval = llvm::ConstantInt::getTrue();
            break;
        case TOKunord:
            skip = true;
            eval = llvm::ConstantInt::getFalse();
            break;

        default:
            assert(0);
        }
        if (!skip)
        {
            LLValue* a = l->getRVal();
            LLValue* b = r->getRVal();
            Logger::cout() << "type 1: " << *a << '\n';
            Logger::cout() << "type 2: " << *b << '\n';
            eval = p->ir->CreateICmp(cmpop, a, b, "tmp");
        }
    }
    else if (t->isfloating())
    {
        llvm::FCmpInst::Predicate cmpop;
        switch(op)
        {
        case TOKlt:
            cmpop = llvm::FCmpInst::FCMP_OLT;break;
        case TOKle:
            cmpop = llvm::FCmpInst::FCMP_OLE;break;
        case TOKgt:
            cmpop = llvm::FCmpInst::FCMP_OGT;break;
        case TOKge:
            cmpop = llvm::FCmpInst::FCMP_OGE;break;
        case TOKunord:
            cmpop = llvm::FCmpInst::FCMP_UNO;break;
        case TOKule:
            cmpop = llvm::FCmpInst::FCMP_ULE;break;
        case TOKul:
            cmpop = llvm::FCmpInst::FCMP_ULT;break;
        case TOKuge:
            cmpop = llvm::FCmpInst::FCMP_UGE;break;
        case TOKug:
            cmpop = llvm::FCmpInst::FCMP_UGT;break;
        case TOKue:
            cmpop = llvm::FCmpInst::FCMP_UEQ;break;
        case TOKlg:
            cmpop = llvm::FCmpInst::FCMP_ONE;break;
        case TOKleg:
            cmpop = llvm::FCmpInst::FCMP_ORD;break;

        default:
            assert(0);
        }
        eval = p->ir->CreateFCmp(cmpop, l->getRVal(), r->getRVal(), "tmp");
    }
    else if (t->ty == Tsarray || t->ty == Tarray)
    {
        Logger::println("static or dynamic array");
        eval = DtoArrayCompare(op,l,r);
    }
    else
    {
        assert(0 && "Unsupported CmpExp type");
    }

    return new DImValue(type, eval);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* EqualExp::toElem(IRState* p)
{
    Logger::print("EqualExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    Type* t = DtoDType(e1->type);
    Type* e2t = DtoDType(e2->type);
    //assert(t == e2t);

    LLValue* eval = 0;

    if (t->isintegral() || t->ty == Tpointer)
    {
        Logger::println("integral or pointer");
        llvm::ICmpInst::Predicate cmpop;
        switch(op)
        {
        case TOKequal:
            cmpop = llvm::ICmpInst::ICMP_EQ;
            break;
        case TOKnotequal:
            cmpop = llvm::ICmpInst::ICMP_NE;
            break;
        default:
            assert(0);
        }
        LLValue* lv = l->getRVal();
        LLValue* rv = r->getRVal();
        if (rv->getType() != lv->getType()) {
            rv = DtoBitCast(rv, lv->getType());
        }
        eval = p->ir->CreateICmp(cmpop, lv, rv, "tmp");
    }
    else if (t->iscomplex())
    {
        Logger::println("complex");
        eval = DtoComplexEquals(loc, op, l, r);
    }
    else if (t->isfloating())
    {
        Logger::println("floating");
        llvm::FCmpInst::Predicate cmpop;
        switch(op)
        {
        case TOKequal:
            cmpop = llvm::FCmpInst::FCMP_OEQ;
            break;
        case TOKnotequal:
            cmpop = llvm::FCmpInst::FCMP_UNE;
            break;
        default:
            assert(0);
        }
        eval = p->ir->CreateFCmp(cmpop, l->getRVal(), r->getRVal(), "tmp");
    }
    else if (t->ty == Tsarray || t->ty == Tarray)
    {
        Logger::println("static or dynamic array");
        eval = DtoArrayEquals(op,l,r);
    }
    else if (t->ty == Tdelegate)
    {
        Logger::println("delegate");
        eval = DtoDelegateEquals(op,l->getRVal(),r->getRVal());
    }
    else if (t->ty == Tstruct)
    {
        Logger::println("struct");
        // when this is reached it means there is no opEquals overload.
        eval = DtoStructEquals(op,l,r);
    }
    else
    {
        assert(0 && "Unsupported EqualExp type");
    }

    return new DImValue(type, eval);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* PostExp::toElem(IRState* p)
{
    Logger::print("PostExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);
    DValue* r = e2->toElem(p);

    LLValue* val = l->getRVal();
    LLValue* post = 0;

    Type* e1type = DtoDType(e1->type);
    Type* e2type = DtoDType(e2->type);

    if (e1type->isintegral())
    {
        assert(e2type->isintegral());
        LLValue* one = llvm::ConstantInt::get(val->getType(), 1, !e2type->isunsigned());
        if (op == TOKplusplus) {
            post = llvm::BinaryOperator::createAdd(val,one,"tmp",p->scopebb());
        }
        else if (op == TOKminusminus) {
            post = llvm::BinaryOperator::createSub(val,one,"tmp",p->scopebb());
        }
    }
    else if (e1type->ty == Tpointer)
    {
        assert(e2type->isintegral());
        LLConstant* minusone = llvm::ConstantInt::get(DtoSize_t(),(uint64_t)-1,true);
        LLConstant* plusone = llvm::ConstantInt::get(DtoSize_t(),(uint64_t)1,false);
        LLConstant* whichone = (op == TOKplusplus) ? plusone : minusone;
        post = llvm::GetElementPtrInst::Create(val, whichone, "tmp", p->scopebb());
    }
    else if (e1type->isfloating())
    {
        assert(e2type->isfloating());
        LLValue* one = DtoConstFP(e1type, 1.0);
        if (op == TOKplusplus) {
            post = llvm::BinaryOperator::createAdd(val,one,"tmp",p->scopebb());
        }
        else if (op == TOKminusminus) {
            post = llvm::BinaryOperator::createSub(val,one,"tmp",p->scopebb());
        }
    }
    else
    assert(post);

    DtoStore(post,l->getLVal());

    return new DImValue(type,val,true);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* NewExp::toElem(IRState* p)
{
    Logger::print("NewExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    assert(newtype);
    Type* ntype = DtoDType(newtype);

    // new class
    if (ntype->ty == Tclass) {
        Logger::println("new class");
        return DtoNewClass((TypeClass*)ntype, this);
    }
    // new dynamic array
    else if (ntype->ty == Tarray)
    {
        Logger::println("new dynamic array: %s", newtype->toChars());
        // get dim
        assert(arguments);
        assert(arguments->dim >= 1);
        if (arguments->dim == 1)
        {
            DValue* sz = ((Expression*)arguments->data[0])->toElem(p);
            // allocate & init
            return DtoNewDynArray(newtype, sz, true);
        }
        else
        {
            size_t ndims = arguments->dim;
            std::vector<DValue*> dims(ndims);
            for (size_t i=0; i<ndims; ++i)
                dims[i] = ((Expression*)arguments->data[i])->toElem(p);
            return DtoNewMulDimDynArray(newtype, &dims[0], ndims, true);
        }
    }
    // new static array
    else if (ntype->ty == Tsarray)
    {
        assert(0);
    }
    // new struct
    else if (ntype->ty == Tstruct)
    {
        Logger::println("new struct on heap: %s\n", newtype->toChars());
        // allocate
        LLValue* mem = DtoNew(newtype);
        // init
        TypeStruct* ts = (TypeStruct*)ntype;
        if (ts->isZeroInit()) {
            DtoAggrZeroInit(mem);
        }
        else {
            assert(ts->sym);
            DtoAggrCopy(mem,ts->sym->ir.irStruct->init);
        }
        return new DImValue(type, mem, false);
    }
    // new basic type
    else
    {
        // allocate
        LLValue* mem = DtoNew(newtype);
        DVarValue tmpvar(newtype, mem, true);

        // default initialize
        Expression* exp = newtype->defaultInit(loc);
        DValue* iv = exp->toElem(gIR);
        DtoAssign(loc, &tmpvar, iv);

        // return as pointer-to
        return new DImValue(type, mem, false);
    }

    assert(0);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DeleteExp::toElem(IRState* p)
{
    Logger::print("DeleteExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* dval = e1->toElem(p);
    Type* et = DtoDType(e1->type);

    // simple pointer
    if (et->ty == Tpointer)
    {
        LLValue* rval = dval->getRVal();
        DtoDeleteMemory(rval);
        if (dval->isVar() && dval->isVar()->lval)
            DtoStore(llvm::Constant::getNullValue(rval->getType()), dval->getLVal());
    }
    // class
    else if (et->ty == Tclass)
    {
        bool onstack = false;
        TypeClass* tc = (TypeClass*)et;
        if (tc->sym->isInterfaceDeclaration())
        {
            DtoDeleteInterface(dval->getRVal());
            onstack = true;
        }
        else if (DVarValue* vv = dval->isVar()) {
            if (vv->var && vv->var->onstack) {
                if (tc->sym->dtors.dim > 0)
                    DtoFinalizeClass(dval->getRVal());
                onstack = true;
            }
        }
        if (!onstack) {
            LLValue* rval = dval->getRVal();
            DtoDeleteClass(rval);
        }
        if (!dval->isThis() && dval->isVar() && dval->isVar()->lval) {
            LLValue* lval = dval->getLVal();
            DtoStore(llvm::Constant::getNullValue(lval->getType()->getContainedType(0)), lval);
        }
    }
    // dyn array
    else if (et->ty == Tarray)
    {
        DtoDeleteArray(dval);
        if (!dval->isSlice())
            DtoSetArrayToNull(dval->getRVal());
    }
    // unknown/invalid
    else
    {
        assert(0 && "invalid delete");
    }

    // no value to return
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ArrayLengthExp::toElem(IRState* p)
{
    Logger::print("ArrayLengthExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);
    return new DImValue(type, DtoArrayLen(u));
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AssertExp::toElem(IRState* p)
{
    Logger::print("AssertExp::toElem: %s\n", toChars());
    LOG_SCOPE;

    // condition
    DValue* cond = e1->toElem(p);

    // create basic blocks
    llvm::BasicBlock* oldend = p->scopeend();
    llvm::BasicBlock* assertbb = llvm::BasicBlock::Create("assert", p->topfunc(), oldend);
    llvm::BasicBlock* endbb = llvm::BasicBlock::Create("noassert", p->topfunc(), oldend);

    // test condition
    LLValue* condval = DtoBoolean(loc, cond);

    // branch
    llvm::BranchInst::Create(endbb, assertbb, condval, p->scopebb());

    // call assert runtime functions
    p->scope() = IRScope(assertbb,endbb);
    DtoAssert(&loc, msg ? msg->toElem(p) : NULL);

    // assert inserts unreachable terminator
//     if (!gIR->scopereturned())
//         llvm::BranchInst::Create(endbb, p->scopebb());

    // rewrite the scope
    p->scope() = IRScope(endbb,oldend);

    // no meaningful return value
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* NotExp::toElem(IRState* p)
{
    Logger::print("NotExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);

    LLValue* b = DtoBoolean(loc, u);

    LLConstant* zero = DtoConstBool(false);
    b = p->ir->CreateICmpEQ(b,zero);

    return new DImValue(type, b);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AndAndExp::toElem(IRState* p)
{
    Logger::print("AndAndExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // allocate a temporary for the final result. failed to come up with a better way :/
    LLValue* resval = 0;
    llvm::BasicBlock* entryblock = &p->topfunc()->front();
    resval = new llvm::AllocaInst(LLType::Int1Ty,"andandtmp",p->topallocapoint());

    DValue* u = e1->toElem(p);

    llvm::BasicBlock* oldend = p->scopeend();
    llvm::BasicBlock* andand = llvm::BasicBlock::Create("andand", gIR->topfunc(), oldend);
    llvm::BasicBlock* andandend = llvm::BasicBlock::Create("andandend", gIR->topfunc(), oldend);

    LLValue* ubool = DtoBoolean(loc, u);
    DtoStore(ubool,resval);
    llvm::BranchInst::Create(andand,andandend,ubool,p->scopebb());

    p->scope() = IRScope(andand, andandend);
    DValue* v = e2->toElem(p);

    LLValue* vbool = DtoBoolean(loc, v);
    LLValue* uandvbool = llvm::BinaryOperator::create(llvm::BinaryOperator::And, ubool, vbool,"tmp",p->scopebb());
    DtoStore(uandvbool,resval);
    llvm::BranchInst::Create(andandend,p->scopebb());

    p->scope() = IRScope(andandend, oldend);

    resval = DtoLoad(resval);
    return new DImValue(type, resval);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* OrOrExp::toElem(IRState* p)
{
    Logger::print("OrOrExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // allocate a temporary for the final result. failed to come up with a better way :/
    LLValue* resval = 0;
    llvm::BasicBlock* entryblock = &p->topfunc()->front();
    resval = new llvm::AllocaInst(LLType::Int1Ty,"orortmp",p->topallocapoint());

    DValue* u = e1->toElem(p);

    llvm::BasicBlock* oldend = p->scopeend();
    llvm::BasicBlock* oror = llvm::BasicBlock::Create("oror", gIR->topfunc(), oldend);
    llvm::BasicBlock* ororend = llvm::BasicBlock::Create("ororend", gIR->topfunc(), oldend);

    LLValue* ubool = DtoBoolean(loc, u);
    DtoStore(ubool,resval);
    llvm::BranchInst::Create(ororend,oror,ubool,p->scopebb());

    p->scope() = IRScope(oror, ororend);
    DValue* v = e2->toElem(p);

    LLValue* vbool = DtoBoolean(loc, v);
    DtoStore(vbool,resval);
    llvm::BranchInst::Create(ororend,p->scopebb());

    p->scope() = IRScope(ororend, oldend);

    resval = new llvm::LoadInst(resval,"tmp",p->scopebb());
    return new DImValue(type, resval);
}

//////////////////////////////////////////////////////////////////////////////////////////

#define BinBitExp(X,Y) \
DValue* X##Exp::toElem(IRState* p) \
{ \
    Logger::print("%sExp::toElem: %s | %s\n", #X, toChars(), type->toChars()); \
    LOG_SCOPE; \
    DValue* u = e1->toElem(p); \
    DValue* v = e2->toElem(p); \
    LLValue* x = llvm::BinaryOperator::create(llvm::Instruction::Y, u->getRVal(), v->getRVal(), "tmp", p->scopebb()); \
    return new DImValue(type, x); \
} \
\
DValue* X##AssignExp::toElem(IRState* p) \
{ \
    Logger::print("%sAssignExp::toElem: %s | %s\n", #X, toChars(), type->toChars()); \
    LOG_SCOPE; \
    DValue* u = e1->toElem(p); \
    DValue* v = e2->toElem(p); \
    LLValue* uval = u->getRVal(); \
    LLValue* vval = v->getRVal(); \
    LLValue* tmp = llvm::BinaryOperator::create(llvm::Instruction::Y, uval, vval, "tmp", p->scopebb()); \
    DtoStore(DtoPointedType(u->getLVal(), tmp), u->getLVal()); \
    return u; \
}

BinBitExp(And,And);
BinBitExp(Or,Or);
BinBitExp(Xor,Xor);
BinBitExp(Shl,Shl);
BinBitExp(Ushr,LShr);

DValue* ShrExp::toElem(IRState* p)
{
    Logger::print("ShrExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    DValue* u = e1->toElem(p);
    DValue* v = e2->toElem(p);
    LLValue* x;
    if (e1->type->isunsigned())
        x = p->ir->CreateLShr(u->getRVal(), v->getRVal(), "tmp");
    else
        x = p->ir->CreateAShr(u->getRVal(), v->getRVal(), "tmp");
    return new DImValue(type, x);
}

DValue* ShrAssignExp::toElem(IRState* p)
{
    Logger::print("ShrAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;
    DValue* u = e1->toElem(p);
    DValue* v = e2->toElem(p);
    LLValue* uval = u->getRVal();
    LLValue* vval = v->getRVal();
    LLValue* tmp;
    if (e1->type->isunsigned())
        tmp = p->ir->CreateLShr(uval, vval, "tmp");
    else
        tmp = p->ir->CreateAShr(uval, vval, "tmp");
    DtoStore(DtoPointedType(u->getLVal(), tmp), u->getLVal());
    return u;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* HaltExp::toElem(IRState* p)
{
    Logger::print("HaltExp::toElem: %s\n", toChars());
    LOG_SCOPE;

    // FIXME: DMD inserts a trap here... we probably should as well !?!

#if 1
    DtoAssert(&loc, NULL);
#else
    // call the new (?) trap intrinsic
    p->ir->CreateCall(GET_INTRINSIC_DECL(trap),"");
    new llvm::UnreachableInst(p->scopebb());
#endif

    // this terminated the basicblock, start a new one
    // this is sensible, since someone might goto behind the assert
    // and prevents compiler errors if a terminator follows the assert
    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* bb = llvm::BasicBlock::Create("afterhalt", p->topfunc(), oldend);
    p->scope() = IRScope(bb,oldend);

    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* DelegateExp::toElem(IRState* p)
{
    Logger::print("DelegateExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    const LLPointerType* int8ptrty = getPtrToType(LLType::Int8Ty);

    LLValue* lval = new llvm::AllocaInst(DtoType(type), "tmpdelegate", p->topallocapoint());

    DValue* u = e1->toElem(p);
    LLValue* uval;
    if (DFuncValue* f = u->isFunc()) {
        assert(f->func);
        LLValue* contextptr = DtoNestedContext(f->func->toParent2()->isFuncDeclaration());
        if (!contextptr)
            uval = LLConstant::getNullValue(getVoidPtrType());
        else
            uval = DtoBitCast(contextptr, getVoidPtrType());
    }
    else {
        DValue* src = u;
        if (ClassDeclaration* cd = u->getType()->isClassHandle())
        {
            Logger::println("context type is class handle");
            if (cd->isInterfaceDeclaration())
            {
                Logger::println("context type is interface");
                src = DtoCastInterfaceToObject(u, ClassDeclaration::object->type);
            }
        }
        uval = src->getRVal();
    }

    Logger::cout() << "context = " << *uval << '\n';

    LLValue* context = DtoGEPi(lval,0,0);
    LLValue* castcontext = DtoBitCast(uval, int8ptrty);
    DtoStore(castcontext, context);

    LLValue* fptr = DtoGEPi(lval,0,1);

    Logger::println("func: '%s'", func->toPrettyChars());

    LLValue* castfptr;
    if (func->isVirtual())
        castfptr = DtoVirtualFunctionPointer(u, func);
    else if (func->isAbstract())
        assert(0 && "TODO delegate to abstract method");
    else if (func->toParent()->isInterfaceDeclaration())
        assert(0 && "TODO delegate to interface method");
    else
    {
        DtoForceDeclareDsymbol(func);
        castfptr = func->ir.irFunc->func;
    }

    castfptr = DtoBitCast(castfptr, fptr->getType()->getContainedType(0));
    DtoStore(castfptr, fptr);

    return new DImValue(type, lval);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* IdentityExp::toElem(IRState* p)
{
    Logger::print("IdentityExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);
    DValue* v = e2->toElem(p);

    Type* t1 = e1->type->toBasetype();

    // handle dynarray specially
    if (t1->ty == Tarray)
        return new DImValue(type, DtoDynArrayIs(op,u,v));
    // also structs
    else if (t1->ty == Tstruct)
        return new DImValue(type, DtoStructEquals(op,u,v));

    // FIXME this stuff isn't pretty
    LLValue* l = u->getRVal();
    LLValue* r = v->getRVal();
    LLValue* eval = 0;

    if (t1->ty == Tdelegate) {
        if (v->isNull()) {
            r = NULL;
        }
        else {
            assert(l->getType() == r->getType());
        }
        eval = DtoDelegateEquals(op,l,r);
    }
    else if (t1->isfloating())
    {
        eval = (op == TOKidentity)
        ?   p->ir->CreateFCmpOEQ(l,r,"tmp")
        :   p->ir->CreateFCmpONE(l,r,"tmp");
    }
    else if (t1->ty == Tpointer)
    {
        if (l->getType() != r->getType()) {
            if (v->isNull())
                r = llvm::ConstantPointerNull::get(isaPointer(l->getType()));
            else
                r = DtoBitCast(r, l->getType());
        }
        eval = (op == TOKidentity)
        ?   p->ir->CreateICmpEQ(l,r,"tmp")
        :   p->ir->CreateICmpNE(l,r,"tmp");
    }
    else {
        eval = (op == TOKidentity)
        ?   p->ir->CreateICmpEQ(l,r,"tmp")
        :   p->ir->CreateICmpNE(l,r,"tmp");
    }
    return new DImValue(type, eval);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CommaExp::toElem(IRState* p)
{
    Logger::print("CommaExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);
    DValue* v = e2->toElem(p);
    assert(e2->type == type);
    return v;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CondExp::toElem(IRState* p)
{
    Logger::print("CondExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    Type* dtype = DtoDType(type);
    const LLType* resty = DtoType(dtype);

    // allocate a temporary for the final result. failed to come up with a better way :/
    llvm::BasicBlock* entryblock = &p->topfunc()->front();
    LLValue* resval = new llvm::AllocaInst(resty,"condtmp",p->topallocapoint());
    DVarValue* dvv = new DVarValue(type, resval, true);

    llvm::BasicBlock* oldend = p->scopeend();
    llvm::BasicBlock* condtrue = llvm::BasicBlock::Create("condtrue", gIR->topfunc(), oldend);
    llvm::BasicBlock* condfalse = llvm::BasicBlock::Create("condfalse", gIR->topfunc(), oldend);
    llvm::BasicBlock* condend = llvm::BasicBlock::Create("condend", gIR->topfunc(), oldend);

    DValue* c = econd->toElem(p);
    LLValue* cond_val = DtoBoolean(loc, c);
    llvm::BranchInst::Create(condtrue,condfalse,cond_val,p->scopebb());

    p->scope() = IRScope(condtrue, condfalse);
    DValue* u = e1->toElem(p);
    DtoAssign(loc, dvv, u);
    llvm::BranchInst::Create(condend,p->scopebb());

    p->scope() = IRScope(condfalse, condend);
    DValue* v = e2->toElem(p);
    DtoAssign(loc, dvv, v);
    llvm::BranchInst::Create(condend,p->scopebb());

    p->scope() = IRScope(condend, oldend);
    return dvv;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ComExp::toElem(IRState* p)
{
    Logger::print("ComExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* u = e1->toElem(p);

    LLValue* value = u->getRVal();
    LLValue* minusone = llvm::ConstantInt::get(value->getType(), -1, true);
    value = llvm::BinaryOperator::create(llvm::Instruction::Xor, value, minusone, "tmp", p->scopebb());

    return new DImValue(type, value);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* NegExp::toElem(IRState* p)
{
    Logger::print("NegExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);

    if (type->iscomplex()) {
        return DtoComplexNeg(loc, type, l);
    }

    LLValue* val = l->getRVal();
    Type* t = DtoDType(type);

    LLValue* zero = 0;
    if (t->isintegral())
        zero = llvm::ConstantInt::get(val->getType(), 0, true);
    else if (t->isfloating()) {
        zero = DtoConstFP(type, 0.0);
    }
    else
        assert(0);

    val = llvm::BinaryOperator::createSub(zero,val,"tmp",p->scopebb());
    return new DImValue(type, val);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CatExp::toElem(IRState* p)
{
    Logger::print("CatExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    Type* t = DtoDType(type);

    bool arrNarr = DtoDType(e1->type) == DtoDType(e2->type);

    // array ~ array
    if (arrNarr)
    {
        return DtoCatArrays(type, e1, e2);
    }
    // array ~ element
    // element ~ array
    else
    {
        return DtoCatArrayElement(type, e1, e2);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* CatAssignExp::toElem(IRState* p)
{
    Logger::print("CatAssignExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* l = e1->toElem(p);

    Type* e1type = DtoDType(e1->type);
    Type* elemtype = DtoDType(e1type->next);
    Type* e2type = DtoDType(e2->type);

    if (e2type == elemtype) {
        DSliceValue* slice = DtoCatAssignElement(l,e2);
        DtoAssign(loc, l, slice);
    }
    else if (e1type == e2type) {
        DSliceValue* slice = DtoCatAssignArray(l,e2);
        DtoAssign(loc, l, slice);
    }
    else
        assert(0 && "only one element at a time right now");

    return l;
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* FuncExp::toElem(IRState* p)
{
    Logger::print("FuncExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    assert(fd);

    if (fd->isNested()) Logger::println("nested");
    Logger::println("kind = %s\n", fd->kind());

    DtoForceDefineDsymbol(fd);

    const LLType* dgty = DtoType(type);
    LLValue* lval = new llvm::AllocaInst(dgty,"dgstorage",p->topallocapoint());

    LLValue* context = DtoGEPi(lval,0,0);
    const LLPointerType* pty = isaPointer(context->getType()->getContainedType(0));
    LLValue* llvmNested = p->func()->decl->ir.irFunc->nestedVar;
    if (llvmNested == NULL) {
        LLValue* nullcontext = llvm::ConstantPointerNull::get(pty);
        DtoStore(nullcontext, context);
    }
    else {
        LLValue* nestedcontext = DtoBitCast(llvmNested, pty);
        DtoStore(nestedcontext, context);
    }

    LLValue* fptr = DtoGEPi(lval,0,1,"tmp",p->scopebb());

    assert(fd->ir.irFunc->func);
    LLValue* castfptr = DtoBitCast(fd->ir.irFunc->func, fptr->getType()->getContainedType(0));
    DtoStore(castfptr, fptr);

    return new DVarValue(type, lval, true);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* ArrayLiteralExp::toElem(IRState* p)
{
    Logger::print("ArrayLiteralExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    // D types
    Type* arrayType = type->toBasetype();
    Type* elemType = arrayType->nextOf()->toBasetype();

    // is dynamic ?
    bool dyn = (arrayType->ty == Tarray);
    // length
    size_t len = elements->dim;
    // store into slice?
    bool sliceInPlace = false;

    // llvm target type
    const LLType* llType = DtoType(arrayType);
    Logger::cout() << (dyn?"dynamic":"static") << " array literal with length " << len << " of D type: '" << arrayType->toChars() << "' has llvm type: '" << *llType << "'\n";

    // llvm storage type
    const LLType* llStoType = LLArrayType::get(DtoType(elemType), len);
    Logger::cout() << "llvm storage type: '" << *llStoType << "'\n";

    // dst pointer
    LLValue* dstMem = 0;
    dstMem = new llvm::AllocaInst(llStoType, "arrayliteral", p->topallocapoint());

    // store elements
    for (size_t i=0; i<len; ++i)
    {
        Expression* expr = (Expression*)elements->data[i];
        LLValue* elemAddr = DtoGEPi(dstMem,0,i,"tmp",p->scopebb());

        // emulate assignment
        DVarValue* vv = new DVarValue(expr->type, elemAddr, true);
        DValue* e = expr->toElem(p);
        DImValue* im = e->isIm();
        if (!im || !im->inPlace()) {
            DtoAssign(loc, vv, e);
        }
    }

    // return storage directly ?
    if (!dyn || (dyn && sliceInPlace))
        return new DImValue(type, dstMem, false);
    // wrap in a slice
    return new DSliceValue(type, DtoConstSize_t(len), DtoGEPi(dstMem,0,0,"tmp"));
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* ArrayLiteralExp::toConstElem(IRState* p)
{
    Logger::print("ArrayLiteralExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    const LLType* t = DtoType(type);
    Logger::cout() << "array literal has llvm type: " << *t << '\n';
    assert(isaArray(t));
    const LLArrayType* arrtype = isaArray(t);

    assert(arrtype->getNumElements() == elements->dim);
    std::vector<LLConstant*> vals(elements->dim, NULL);
    for (unsigned i=0; i<elements->dim; ++i)
    {
        Expression* expr = (Expression*)elements->data[i];
        vals[i] = expr->toConstElem(p);
    }

    return llvm::ConstantArray::get(arrtype, vals);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* StructLiteralExp::toElem(IRState* p)
{
    Logger::print("StructLiteralExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    const LLType* llt = DtoType(type);

    LLValue* mem = 0;

    LLValue* sptr = new llvm::AllocaInst(llt,"tmpstructliteral",p->topallocapoint());


    // num elements in literal
    unsigned n = elements->dim;

    // unions might have different types for each literal
    if (sd->ir.irStruct->hasUnions) {
        // build the type of the literal
        std::vector<const LLType*> tys;
        for (unsigned i=0; i<n; ++i) {
            Expression* vx = (Expression*)elements->data[i];
            if (!vx) continue;
            tys.push_back(DtoType(vx->type));
        }
        const LLStructType* t = LLStructType::get(tys);
        if (t != llt) {
            if (getABITypeSize(t) != getABITypeSize(llt)) {
                Logger::cout() << "got size " << getABITypeSize(t) << ", expected " << getABITypeSize(llt) << '\n';
                assert(0 && "type size mismatch");
            }
            sptr = DtoBitCast(sptr, getPtrToType(t));
            Logger::cout() << "sptr type is now: " << *t << '\n';
        }
    }

    // build
    unsigned j = 0;
    for (unsigned i=0; i<n; ++i)
    {
        Expression* vx = (Expression*)elements->data[i];
        if (!vx) continue;

        Logger::cout() << "getting index " << j << " of " << *sptr << '\n';
        LLValue* arrptr = DtoGEPi(sptr,0,j);
        DValue* darrptr = new DVarValue(vx->type, arrptr, true);

        DValue* ve = vx->toElem(p);

        if (!ve->inPlace())
            DtoAssign(loc, darrptr, ve);

        j++;
    }

    return new DImValue(type, sptr);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* StructLiteralExp::toConstElem(IRState* p)
{
    Logger::print("StructLiteralExp::toConstElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    unsigned n = elements->dim;
    std::vector<LLConstant*> vals(n, NULL);

    for (unsigned i=0; i<n; ++i)
    {
        Expression* vx = (Expression*)elements->data[i];
        vals[i] = vx->toConstElem(p);
    }

    assert(DtoDType(type)->ty == Tstruct);
    const LLType* t = DtoType(type);
    const LLStructType* st = isaStruct(t);
    return llvm::ConstantStruct::get(st,vals);
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* InExp::toElem(IRState* p)
{
    Logger::print("InExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    DValue* key = e1->toElem(p);
    DValue* aa = e2->toElem(p);

    return DtoAAIn(loc, type, aa, key);
}

DValue* RemoveExp::toElem(IRState* p)
{
    Logger::print("RemoveExp::toElem: %s\n", toChars());
    LOG_SCOPE;

    DValue* aa = e1->toElem(p);
    DValue* key = e2->toElem(p);

    DtoAARemove(loc, aa, key);

    return NULL; // does not produce anything useful
}

//////////////////////////////////////////////////////////////////////////////////////////

DValue* AssocArrayLiteralExp::toElem(IRState* p)
{
    Logger::print("AssocArrayLiteralExp::toElem: %s | %s\n", toChars(), type->toChars());
    LOG_SCOPE;

    assert(keys);
    assert(values);
    assert(keys->dim == values->dim);

    Type* aatype = DtoDType(type);
    Type* vtype = aatype->next;
    const LLType* aalltype = DtoType(type);

    // it should be possible to avoid the temporary in some cases
    LLValue* tmp = new llvm::AllocaInst(aalltype,"aaliteral",p->topallocapoint());
    DValue* aa = new DVarValue(type, tmp, true);
    DtoStore(LLConstant::getNullValue(aalltype), tmp);

    const size_t n = keys->dim;
    for (size_t i=0; i<n; ++i)
    {
        Expression* ekey = (Expression*)keys->data[i];
        Expression* eval = (Expression*)values->data[i];

        Logger::println("(%u) aa[%s] = %s", i, ekey->toChars(), eval->toChars());

        // index
        DValue* key = ekey->toElem(p);
        DValue* mem = DtoAAIndex(loc, vtype, aa, key);

        // store
        DValue* val = eval->toElem(p);
        DtoAssign(loc, mem, val);
    }

    return aa;
}

//////////////////////////////////////////////////////////////////////////////////////////

#define STUB(x) DValue *x::toElem(IRState * p) {error("Exp type "#x" not implemented: %s", toChars()); fatal(); return 0; }
STUB(Expression);
STUB(DotTypeExp);
STUB(TypeDotIdExp);
STUB(ScopeExp);
STUB(TypeExp);
STUB(BoolExp);
STUB(TupleExp);

#define CONSTSTUB(x) LLConstant* x::toConstElem(IRState * p) {error("const Exp type "#x" not implemented: '%s' type: '%s'", toChars(), type->toChars()); fatal(); return NULL; }
CONSTSTUB(Expression);
CONSTSTUB(AssocArrayLiteralExp);

unsigned Type::totym() { return 0; }

type * Type::toCtype()
{
    assert(0);
    return 0;
}

type * Type::toCParamtype()
{
    assert(0);
    return 0;
}
Symbol * Type::toSymbol()
{
    assert(0);
    return 0;
}

type *
TypeTypedef::toCtype()
{
    assert(0);
    return 0;
}

type *
TypeTypedef::toCParamtype()
{
    assert(0);
    return 0;
}

void
TypedefDeclaration::toDebug()
{
    assert(0);
}


type *
TypeEnum::toCtype()
{
    assert(0);
    return 0;
}

type *
TypeStruct::toCtype()
{
    assert(0);
    return 0;
}

void
StructDeclaration::toDebug()
{
    assert(0);
}

Symbol * TypeClass::toSymbol()
{
    assert(0);
    return 0;
}

unsigned TypeFunction::totym()
{
    assert(0);
    return 0;
}

type * TypeFunction::toCtype()
{
    assert(0);
    return 0;
}

type * TypeSArray::toCtype()
{
    assert(0);
    return 0;
}

type *TypeSArray::toCParamtype()
{
    assert(0);
    return 0;
}

type * TypeDArray::toCtype()
{
    assert(0);
    return 0;
}

type * TypeAArray::toCtype()
{
    assert(0);
    return 0;
}

type * TypePointer::toCtype()
{
    assert(0);
    return 0;
}

type * TypeDelegate::toCtype()
{
    assert(0);
    return 0;
}

type * TypeClass::toCtype()
{
    assert(0);
    return 0;
}

void ClassDeclaration::toDebug()
{
    assert(0);
}

//////////////////////////////////////////////////////////////////////////////

void
EnumDeclaration::toDebug()
{
    assert(0);
}

int Dsymbol::cvMember(unsigned char*)
{
    assert(0);
    return 0;
}
int EnumDeclaration::cvMember(unsigned char*)
{
    assert(0);
    return 0;
}
int FuncDeclaration::cvMember(unsigned char*)
{
    assert(0);
    return 0;
}
int VarDeclaration::cvMember(unsigned char*)
{
    assert(0);
    return 0;
}
int TypedefDeclaration::cvMember(unsigned char*)
{
    assert(0);
    return 0;
}

void obj_includelib(char*)
{
// FIXME: we want to support pragma(lib)
}

void backend_init()
{
    // now lazily loaded
    //LLVM_D_InitRuntime();
}

void backend_term()
{
    LLVM_D_FreeRuntime();
}

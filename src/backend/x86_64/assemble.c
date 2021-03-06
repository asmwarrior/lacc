#if !AMALGAMATION
# define INTERNAL
# define EXTERNAL extern
#endif
#include "abi.h"
#include "assemble.h"

#include <assert.h>
#include <ctype.h>
#include <stdarg.h>

#define SUFFIX(w) ((w) == 1 ? 'b' : (w) == 2 ? 'w' : (w) == 4 ? 'l' : 'q')
#define SSESFX(w) ((w) == 4 ? 's' : 'd')
#define X87SFX(w) ((w) == 4 ? 's' : (w) == 8 ? 'l' : 't')
#define X87IFX(w) ((w) == 2 ? 's' : (w) == 4 ? 'l' : 'q')

#define I0(instr)           out("%s\n", instr)
#define I1(instr, a)        out("%s\t%s\n", instr, a)
#define I2(instr, a, b)     out("%s\t%s, %s\n", instr, a, b)
#define C1(instr, cc, a)    out("%s%s\t%s\n", instr, tttn_text(cc), a)
#define U1(instr, w, a)     out("%s%c\t%s\n", instr, SUFFIX(w), a)
#define U2(instr, w, a, b)  out("%s%c\t%s, %s\n", instr, SUFFIX(w), a, b)
#define X1(instr, w, a)     out("%s%c\t%s\n", instr, X87SFX(w), a);
#define Y1(instr, w, a)     out("%s%c\t%s\n", instr, X87IFX(w), a);
#define SSE2(instr, w, a, b)  out("%s%c\t%s, %s\n", instr, SSESFX(w), a, b)

#define MAX_OPERAND_TEXT_LENGTH 256

static const struct symbol *current_symbol;

static FILE *asm_output;

static void out(const char *s, ...)
{
    va_list args;

    va_start(args, s);
    vfprintf(asm_output, s, args);
    va_end(args);
}

static enum section {
    SECTION_NONE,
    SECTION_TEXT,
    SECTION_DATA,
    SECTION_RODATA
} current_section = SECTION_NONE;

static void set_section(enum section section)
{
    if (section != current_section) switch (section) {
    case SECTION_TEXT:
        out("\t.text\n");
        break;
    case SECTION_DATA:
        out("\t.data\n");
        break;
    case SECTION_RODATA:
        out("\t.section\t.rodata\n");
        break;
    default: break;
    }

    current_section = section;
}

static const char *reg_name[] = {
    "%al",   "%ax",   "%eax",  "%rax",
    "%cl",   "%cx",   "%ecx",  "%rcx",
    "%dl",   "%dx",   "%edx",  "%rdx",
    "%bl",   "%bx",   "%ebx",  "%rbx",
    "%spl",  "%sp",   "%esp",  "%rsp",
    "%bpl",  "%bp",   "%ebp",  "%rbp",
    "%sil",  "%si",   "%esi",  "%rsi",
    "%dil",  "%di",   "%edi",  "%rdi",
    "%r8b",  "%r8w",  "%r8d",  "%r8",
    "%r9b",  "%r9w",  "%r9d",  "%r9",
    "%r10b", "%r10w", "%r10d", "%r10",
    "%r11b", "%r11w", "%r11d", "%r11",
    "%r12b", "%r12w", "%r12d", "%r12",
    "%r13b", "%r13w", "%r13d", "%r13",
    "%r14b", "%r14w", "%r14d", "%r14",
    "%r15b", "%r15w", "%r15d", "%r15"
};

static const char *xmm_name[] = {
    "%xmm0",  "%xmm1",  "%xmm2",  "%xmm3",
    "%xmm4",  "%xmm5",  "%xmm6",  "%xmm7",
    "%xmm8",  "%xmm9",  "%xmm10", "%xmm11",
    "%xmm12", "%xmm13", "%xmm14", "%xmm15"
};

static const char *x87_name[] = {
    "%st(0)", "%st(1)", "%st(2)", "%st(3)",
    "%st(4)", "%st(5)", "%st(6)", "%st(7)"
};

static const char *mnemonic(struct registr reg)
{
    int i, j;

    if (reg.r == IP) {
        assert(reg.width == 8);
        return "%rip";
    } else if (reg.r < XMM0) {
        i = 4 * (reg.r - 1);
        j = reg.width - 1;

        if (j == 3) j = 2;
        if (j == 7) j = 3;

        return reg_name[i + j];
    } else if (reg.r < ST0) {
        return xmm_name[reg.r - XMM0];
    } else {
        i = x87_stack_pos(reg.r);
        return x87_name[i];
    }
}

static const char *asm_address(struct address addr)
{
    static char buf[MAX_OPERAND_TEXT_LENGTH];

    struct registr reg = {0};
    int w = 0;

    reg.width = 8;
    if (addr.sym) {
        w += sprintf(buf + w, "%s", sym_name(addr.sym));
        switch (addr.type) {
        case ADDR_GLOBAL_OFFSET:
            assert(addr.displacement == 0);
            w += sprintf(buf + w, "@GOTPCREL");
            break;
        case ADDR_PLT:
            assert(addr.displacement == 0);
            w += sprintf(buf + w, "@PLT");
            break;
        default:
            if (addr.displacement != 0) {
                w += sprintf(buf + w, "%s%d",
                    (addr.displacement > 0) ? "+" : "", addr.displacement);
            }
            break;
        }
    } else if (addr.displacement != 0) {
        w += sprintf(buf, "%d", addr.displacement);
    }

    if (addr.base) {
        reg.r = addr.base;
        w += sprintf(buf + w, "(%s", mnemonic(reg));
        if (addr.index) {
            reg.r = addr.index;
            w += sprintf(buf + w, ",%s,%d", mnemonic(reg), addr.scale);
        } else assert(!addr.scale);
        sprintf(buf + w, ")");
    } else if (addr.index) {
        reg.r = addr.index;
        sprintf(buf + w, "(,%s,%d)", mnemonic(reg), addr.scale);
    } else assert(!addr.scale);

    return buf;
}

static const char *immediate(struct immediate imm, int *size)
{
    static char buf[MAX_OPERAND_TEXT_LENGTH];

    if (imm.type == IMM_INT) {
        *size = imm.width;
        if (imm.width < 8) {
            sprintf(buf, "$%d",
                (imm.width == 1) ? imm.d.byte :
                (imm.width == 2) ? imm.d.word : imm.d.dword);
        } else {
            sprintf(buf, "$%ld", imm.d.qword);
        }

        return buf;
    }

    assert(imm.type == IMM_ADDR);
    assert(imm.d.addr.sym);
    assert(imm.d.addr.sym->symtype != SYM_STRING_VALUE);

    *size = 8;
    return asm_address(imm.d.addr);
}

static const char *tttn_text(enum tttn cc)
{
    switch (cc) {
    default: assert(0);
    case CC_O: return "o";
    case CC_NO: return "no";
    case CC_NAE: return "nae";
    case CC_AE: return "ae";
    case CC_E: return "e";
    case CC_NE: return "ne";
    case CC_NA: return "na";
    case CC_A: return "a";
    case CC_S: return "s";
    case CC_NS: return "ns";
    case CC_P: return "p";
    case CC_NP: return "np";
    case CC_NGE: return "nge";
    case CC_GE: return "ge";
    case CC_NG: return "ng";
    case CC_G: return "g";
    }
}

INTERNAL void asm_init(FILE *output, const char *file)
{
    asm_output = output;
    if (file) {
        out("\t.file\t\"%s\"\n", file);
    }
}

INTERNAL int asm_symbol(const struct symbol *sym)
{
    const char *name;
    size_t size;

    /*
     * Labels stay in the same function context, otherwise flush to
     * write any end of function metadata.
     */
    if (sym->symtype != SYM_LABEL) {
        asm_flush();
        current_symbol = sym;
    }

    name = sym_name(sym);
    size = size_of(sym->type);
    switch (sym->symtype) {
    case SYM_TENTATIVE:
        assert(is_object(sym->type));
        if (sym->linkage == LINK_INTERN)
            out("\t.local\t%s\n", name);
        out("\t.comm\t%s,%lu,%lu\n", name, size, type_alignment(sym->type));
        break;
    case SYM_DEFINITION:
        if (is_function(sym->type)) {
            set_section(SECTION_TEXT);
            if (sym->linkage == LINK_EXTERN)
                out("\t.globl\t%s\n", name);
            out("\t.type\t%s, @function\n", name);
            out("%s:\n", name);
        } else {
            set_section(SECTION_DATA);
            if (sym->linkage == LINK_EXTERN)
                out("\t.globl\t%s\n", name);
            out("\t.align\t%d\n", sym_alignment(sym));
            out("\t.type\t%s, @object\n", name);
            out("\t.size\t%s, %lu\n", name, size);
            out("%s:\n", name);
        }
        break;
    case SYM_STRING_VALUE:
        set_section(SECTION_DATA);
        out("\t.align\t%d\n", sym_alignment(sym));
        out("\t.type\t%s, @object\n", name);
        out("\t.size\t%s, %lu\n", name, size);
        out("%s:\n", name);
        out("\t.string\t");
        fprintstr(asm_output, sym->value.string);
        out("\n");
        break;
    case SYM_CONSTANT:
        set_section(SECTION_RODATA);
        out("\t.align\t%d\n", sym_alignment(sym));
        out("%s:\n", name);
        if (is_float(sym->type)) {
            out("\t.long\t%lu\n", sym->value.constant.u & 0xFFFFFFFFu);
        } else if (is_double(sym->type)) {
            out("\t.quad\t%ld\n", sym->value.constant.i);
        } else {
            union {
                long double ld;
                long i[2];
            } conv = {0};
            assert(is_long_double(sym->type));
            conv.ld = sym->value.constant.ld;
            out("\t.quad\t%ld\n", conv.i[0]);
            out("\t.quad\t%ld\n", conv.i[1] & 0xFFFF);
        }
        break;
    case SYM_LABEL:
        out("%s:\n", name);
        break;
    default:
        break;
    }

    return 0;
}

INTERNAL int asm_text(struct instruction instr)
{
    int ws = 0,
        wd = 0;
    const char
        *source = NULL,
        *destin = NULL;

    switch (instr.optype) {
    case OPT_REG:
    case OPT_REG_REG:
    case OPT_REG_MEM:
        ws = instr.source.width;
        source = mnemonic(instr.source.reg);
        break;
    case OPT_IMM:
    case OPT_IMM_REG:
    case OPT_IMM_MEM:
        source = immediate(instr.source.imm, &ws);
        break;
    case OPT_MEM:
    case OPT_MEM_REG:
        ws = instr.source.width;
        source = asm_address(instr.source.mem.addr);
        break;
    default:
        break;
    }

    switch (instr.optype) {
    case OPT_REG_REG:
    case OPT_MEM_REG:
    case OPT_IMM_REG:
        wd = instr.dest.width;
        destin = mnemonic(instr.dest.reg);
        break;
    case OPT_REG_MEM:
    case OPT_IMM_MEM:
        wd = instr.dest.width;
        destin = asm_address(instr.dest.mem.addr);
        break;
    default:
        break;
    }

    out("\t");
    switch (instr.prefix) {
    case PREFIX_REP: out("rep "); break;
    case PREFIX_REPNE: out("repne "); break;
    default: break;
    }

    switch (instr.opcode) {
    case INSTR_ADD:      U2("add", wd, source, destin); break;
    case INSTR_ADDS:     SSE2("adds", wd, source, destin); break;
    case INSTR_CVTS2S:
        if (ws == 4 && wd == 8) {
            I2("cvtss2sd", source, destin);
        } else {
            assert(ws == 8 && wd == 4);
            I2("cvtsd2ss", source, destin);
        }
        break;
    case INSTR_CVTSI2S: 
        if (wd == 4) {
            U2("cvtsi2ss", ws, source, destin);
        } else {
            assert(wd == 8);
            U2("cvtsi2sd", ws, source, destin);
        }
        break;
    case INSTR_CVTTS2SI:
        if (ws == 4) {
            U2("cvttss2si", wd, source, destin);
        } else {
            assert(ws == 8);
            U2("cvttsd2si", wd, source, destin);
        }
        break;
    case INSTR_Cxy:
        if (instr.source.width == 8) I0("cqo");
        else {
            assert(instr.source.width == 4);
            I0("cdq");
        }
        break;
    case INSTR_DIV:      U1("div", ws, source); break;
    case INSTR_DIVS:     SSE2("divs", wd, source, destin); break;
    case INSTR_SUB:      U2("sub", wd, source, destin); break;
    case INSTR_SUBS:     SSE2("subs", wd, source, destin); break;
    case INSTR_NOT:      U1("not", ws, source); break;
    case INSTR_MUL:      U1("mul", ws, source); break;
    case INSTR_XOR:      U2("xor", wd, source, destin); break;
    case INSTR_AND:      U2("and", wd, source, destin); break;
    case INSTR_OR:       U2("or", wd, source, destin); break;
    case INSTR_SHL:      U2("shl", wd, source, destin); break;
    case INSTR_SHR:      U2("shr", wd, source, destin); break;
    case INSTR_SAR:      U2("sar", wd, source, destin); break;
    case INSTR_IDIV:     U1("idiv", ws, source); break;
    case INSTR_MOV:      U2("mov", wd, source, destin); break;
    case INSTR_MOVZX:
        assert(ws == 1 || ws == 2);
        assert(ws < wd);
        U2((ws == 1) ? "movzb" : "movzw", wd, source, destin);
        break;
    case INSTR_MOVSX:
        assert(ws == 1 || ws == 2 || ws == 4);
        assert(ws < wd);
        U2((ws == 1) ? "movsb" : (ws == 2) ? "movsw" : "movsl",
            wd, source, destin);
        break;
    case INSTR_MOVAP:    SSE2("movap", ws, source, destin); break;
    case INSTR_MOVS:     SSE2("movs", wd, source, destin); break;
    case INSTR_MULS:     SSE2("muls", wd, source, destin); break;
    case INSTR_SETcc:    C1("set", instr.cc, source); break;
    case INSTR_TEST:     U2("test", wd, source, destin); break;
    case INSTR_UCOMIS:   SSE2("ucomis", wd, source, destin); break;
    case INSTR_CMP:      U2("cmp", wd, source, destin); break;
    case INSTR_LEA:      U2("lea", wd, source, destin); break;
    case INSTR_PUSH:     U1("push", ws, source); break;
    case INSTR_POP:      U1("pop", ws, source); break;
    case INSTR_PXOR:     I2("pxor", source, destin); break;
    case INSTR_JMP:      I1("jmp", source); break;
    case INSTR_Jcc:      C1("j", instr.cc, source); break;
    case INSTR_CALL:
        if (instr.optype == OPT_REG)
            out("\tcall\t*%s\n", source);
        else
            I1("call", source);
        break;
    case INSTR_LEAVE:    I0("leave"); break;
    case INSTR_RET:      I0("ret"); break;
    case INSTR_MOV_STR:  out("movs%c\n", SUFFIX(instr.source.width)); break;
    case INSTR_FLD:      X1("fld", ws, source); break;
    case INSTR_FILD:     Y1("fild", ws, source); break;
    case INSTR_FSTP:
        if (instr.optype == OPT_REG) {
            I1("fstp", source);
        } else {
            X1("fstp", ws, source);
        }
        break;
    case INSTR_FXCH:     I1("fxch", source); break;
    case INSTR_FNSTCW:   I1("fnstcw", source); break;
    case INSTR_FLDCW:    I1("fldcw", source); break;
    case INSTR_FISTP:    Y1("fistp", ws, source); break;
    case INSTR_FUCOMIP:  I1("fucomip", source); break;
    case INSTR_FADDP:    I1("faddp", source); break;
    case INSTR_FSUBRP:   I1("fsubrp", source); break;
    case INSTR_FMULP:    I1("fmulp", source); break;
    case INSTR_FDIVRP:   I1("fdivrp", source); break;
    }

    return 0;
}

INTERNAL int asm_data(struct immediate data)
{
    switch (data.type) {
    case IMM_INT:
        if (data.width == 1)
            out("\t.byte\t%d\n", data.d.byte);
        else if (data.width == 2)
            out("\t.short\t%d\n", data.d.word);
        else if (data.width == 4)
            out("\t.int\t%d\n", data.d.dword);
        else {
            assert(data.width == 8);
            out("\t.quad\t%ld\n", data.d.qword);
        }
        break;
    case IMM_ADDR:
        assert(data.d.addr.sym);
        if (data.d.addr.displacement) {
            out("\t.quad\t%s%s%d\n", sym_name(data.d.addr.sym),
                data.d.addr.displacement < 0 ? "" : "+",
                data.d.addr.displacement);
        } else
            out("\t.quad\t%s\n", sym_name(data.d.addr.sym));
        break;
    case IMM_STRING:
        if (data.width == data.d.string.len) {
            out("\t.ascii\t");
        } else {
            assert(data.width == data.d.string.len + 1);
            out("\t.string\t");
        }
        fprintstr(asm_output, data.d.string);
        out("\n");
        break;
    }
    return 0;
}

INTERNAL int asm_flush(void)
{
    const char *name;

    if (current_symbol
        && is_function(current_symbol->type)
        && current_symbol->symtype == SYM_DEFINITION)
    {
        name = sym_name(current_symbol);
        out("\t.size\t%s, .-%s\n", name, name);
    }

    current_symbol = NULL;
    current_section = SECTION_NONE;
    return 0;
}

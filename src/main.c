// TODO: LARGE file support (~65505 or OS_VAR_MAX_SIZE)
// Few possible ways of doing this:
// - Get a contiguous piece of memory 65505 bytes large.
//   One candidate for this is reposessing GFX's draw-buffer,
//   and changing our renderer to accomodate not being double-buffered.
//   Another candidate is "os_MemChk" but this seems to usually be a value less than 65505 on a healthy calculator.
//   and presumably autosave must be disabled as it may be using that section of memory.
//   
// - Page memory in as we need it, and operate on one section of the file at a time
//   (ti_OpenVar("a") may let me overwrite specific parts of the file at once...)
//   
//   I speculate I would want to try overlapping pages like this
//   +-------------------+
//   |  SECTION 1 (0-32k)|  
//   |+------------------+-+
//   ++------------------+ |
//    | SECTION 2 (16-48K) |
//   ++------------------+ |
//   |+------------------+-+
//   | SECTION 3 (32-64k)|
//   +-------------------+
//   Because then we can ensure operating on one at a time.
//   So there need not be a situation where we need to operate on two
//   sections at once on a boundary.
//   But I need to actually experiment with this to see what's actually good.

// TODO: Some performance concerns
// - Rendering a whole screen full of glyphs takes a very long time (~200ms).
//   May be good to have a partial-renderer, or see if a bespoke font renderer
//   with few requirements will be faster.

// TODO: "adriweb: it's a nice ide feature to be able to list labels and instant-jump to them"

// TODO: Maybe it'd be cool if you could goto string. Like
// if you put a comment on a line starting with `"` you could 
// go to those commented lines quickly
// TODO: User-space favorite tokens from catalog
// TODO: hotkey ideas
// - Delete from cursor to end of line
// - Delete from cursor to start of line
// - Goto next/previous occurance of letter in line (like VI's f/t/F/T)
// - Go to top/bottom of screen without scrolling (2nd -> up/down)
//   (probably a bad idea to use 2nd for this because sometimes it's nice to be in a different mode and not worry about differing behavior)

#include <fileioc.h>
#include <ti/screen.h>
#include <ti/tokens.h>
#include <keypadc.h>
#include <graphx.h>
#include <fontlibc.h>
#include <time.h>
#include <sys/timers.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint24_t u24;
typedef int8_t s8;
typedef int16_t s16;
typedef int24_t s24;
typedef int s24;

#define max(a,b) (((a) >= (b)) ? (a) : (b))
#define min(a,b) (((a) <  (b)) ? (a) : (b))
// NOTE: for explicit syntax like cast(u8)
#define cast(type) (type)
// NOTE: To semantically talk about 0-pointers as "null"
#define null 0
#define I_KNOW_ITS_UNUSED(var) ((void)(var))

void copy(void *src, void *dest, s24 count);
void zero(void *dest, s24 count);
int run_prgm_callback(void *data_, int retval);
void update(void);
void blit_loading_indicator(void);
void render(void);
void load_program(char *name);
void save_program(bool are_we_exiting_so_we_should_do_a_final_archiving_of_the_variable);
void draw_string(char* str, u24 x, u8 y);
void draw_string_max_chars(char* str, u24 max, u24 x, u8 y);
void update_input(void);
u8 get_token_size(s24 position_in_program);
typedef struct Range { s24 min; s24 max; } Range;
Range get_selecting_range(void);
s24 calculate_line_y(s24 token_offset);
s24 calculate_cursor_y(void);

// https://merthsoft.com/linkguide/ti83+/tokens.html#note1
// https://merthsoft.com/linkguide/ti83+/vars.html#equation
// https://merthsoft.com/linkguide/ti83+/fformat.html
// http://tibasicdev.wikidot.com/miscellaneous-tokens
// ^ tibasicdev has more accurate token tables, but worse formatting.
//   it also has an inaccuracy on "CLASSIC" token?

// get token string
// https://ce-programming.github.io/toolchain/libraries/fileioc.html?highlight=gettoken#_CPPv417ti_GetTokenStringPPvP7uint8_tPj
// http://tibasicdev.wikidot.com/83lgfont
// https://github.com/RoccoLoxPrograms/OSFonts/blob/main/LargeFontCodes.png

// https://ce-programming.github.io/toolchain/libraries/fontlibc.html#creating-fonts
// http://hukka.ncn.fi/?fony

static const u8 editor_font_data[] = {
    #include "aether_editor_fnt.h"
};
const fontlib_font_t *editor_font = (fontlib_font_t*)editor_font_data;

#ifdef DEBUG
    #include <debug.h>
    #define log dbg_printf
    #define assert(condition, ...) { if(!(condition)) { \
        log("Assertion failed @ %d:%s! Condition:\n" #condition "\n", __LINE__, __FUNCTION__);\
        log(__VA_ARGS__); \
        log("\n"); \
        } }
    typedef struct Delta Delta;
    void dump_delta(char *title, Delta *delta);
    void dump(char *dump_name);
    void dump_deltas(char *title); // NOTE: forward declared... C is ugly
#else
    #define log(...)
    #define assert(...)
    #define dump(...)
    #define dump_delta(...)
    #define dump_deltas(...)
#endif

#define LINEBREAK 0x3F
#define SPACE 0x29
#define LBL 0xD6

#define FONT_WIDTH 7
#define FONT_HEIGHT 8

#define ARRLEN(var) (sizeof((var)) / sizeof((var)[0]))

#define change_indentation_based_on_byte(indentation, byte) \
    if(byte == 0xCF || (byte >= 0xD1 && byte <= 0xD3)) { \
        indentation += 1; \
    } else if(byte == 0xD4) { \
        indentation -= 1; \
        if(indentation < 0) { indentation = 0; } \
    }

typedef struct TokenList {
    char *name;
    s16 name_count;
    const u16 *tokens;
    s16  tokens_count;
} TokenList;

typedef struct TokenDirectory {
    s8 list_count;
    TokenList lists[27];
} TokenDirectory;


/*

// NOTE: For scraping off of http://tibasicdev.wikidot.com/one-byte-tokens
// where temp0 is the <tbody>
// var tokens = []; var firstByte = "EF"; var table = temp0; var chars = Array(10); for(var i in table.children) { var r = table.children[i]; if(!r.children) { continue; } var second_char = ""; for(var j in r.children) { var c = r.children[j]; if(!c.children) { continue; } if(j == 0) { second_char = c.innerText; continue; } if(i == 1) { console.log(j); chars[j] = c.innerText; } else if(i > 1) { tokens.push({n:chars[j] + second_char + firstByte,token:c.innerText}) } }} console.log(tokens); var tokens_per = Array(27); for(var i in tokens) { var index = 26; if(tokens[i].token.length > 0) { var first = tokens[i].token.toLowerCase().charCodeAt(0); if(first >= "a".charCodeAt(0) && first <= "z".charCodeAt(0)) { index = first - "a".charCodeAt(0); } } if(tokens_per[index] === undefined) { tokens_per[index] = []; } tokens_per[index].push(tokens[i]); } console.log(tokens_per); var str = "\n"; for(var i in tokens_per) { if(tokens_per[i][0].token.length <= 0) { continue; } str += tokens_per[i][0].token[0].toUpperCase() + " [ "; for(var j in tokens_per[i]) { if(tokens_per[i][j].token.startsWith("2-byte")) { continue; } str += "{ n:0x" + tokens_per[i][j].n + ", t:\""+escape(tokens_per[i][j].token)+"\" }, "; } str += " ] \n"; } console.log(str);

// All
A [ {n:0x087E, t:"AxesOn" }, {n:0x097E, t:"AxesOff" }, { n:0x1662, t:"a" },{ n:0x68EF, t:"Asm84CPrgm" }, { n:0x3AEF, t:"AUTO" }, { n:0x7AEF, t:"Asm84CEPrgm" }, { n:0xB0BB, t:"a" }, { n:0x68BB, t:"Archive" }, { n:0x6ABB, t:"Asm%28" }, { n:0x6BBB, t:"AsmComp%28" }, { n:0x6CBB, t:"AsmPrgm" }, { n:0x40, t:"and" }, { n:0x41, t:"A" }, { n:0x72, t:"Ans" }, { n:0x14, t:"augment%28" }, { n:0xB2, t:"abs%28" }, { n:0x28BB, t:"angle%28" }, { n:0x59BB, t:"ANOVA%28" }, { n:0x4FBB, t:"a+bi" },  ] ,
B [ { n:0x1762, t:"b" },{ n:0x42, t:"B" }, { n:0x05, t:"Boxplot" }, { n:0x02BB, t:"bal%28" }, { n:0x15BB, t:"binompdf%28" }, { n:0x16BB, t:"binomcdf%28" }, { n:0xB1BB, t:"b" }, { n:0x41EF, t:"BLUE" }, { n:0x43EF, t:"BLACK" }, { n:0x64EF, t:"BackgroundOff" }, { n:0x47EF, t:"BROWN" }, { n:0x5BEF, t:"BackgroundOn" }, { n:0x6CEF, t:"BorderColor" }, ],
C [ {n:0x3163, t:"C/Y" }, {n:0x047E, t:"CoordOn" }, {n:0x057E, t:"CoordOff" }, {n:0x067E, t:"Connected" }, { n:0x1862, t:"c" },{ n:0x43, t:"C" }, { n:0x2E, t:"CubicReg" }, { n:0xE1, t:"ClrHome" }, { n:0xC4, t:"cos%28" }, { n:0x85, t:"ClrDraw" }, { n:0xA5, t:"Circle%28" }, { n:0xC5, t:"cos%u05BF%B9%28" }, { n:0xCA, t:"cosh%28" }, { n:0xFA, t:"ClrList" }, { n:0xCB, t:"cosh%u05BF%B9%28" }, { n:0xFB, t:"ClrTable" }, { n:0x52BB, t:"ClrAllLists" }, { n:0x25BB, t:"conj%28" }, { n:0x57BB, t:"Clear%20Entries" }, { n:0x29BB, t:"cumSum%28" }, { n:0xB2BB, t:"c" }, { n:0x6DBB, t:"compiled%20asm" }, { n:0x10EF, t:"ClockOn" }, { n:0x02EF, t:"checkTmr%28" }, { n:0x37EF, t:"CLASSIC" }, { n:0x69EF, t:"compiled%20asm%20%28CSE%29" }, { n:0x0FEF, t:"ClockOff" }, { n:0xA1EF, t:"Cut%20Line" }, { n:0xA2EF, t:"Copy%20Line" }, { n:0x93EF, t:"CENTER" }, { n:0x7BEF, t:"compiled%20asm%20%28CE%29" }, ],
D [ {n:0x077E, t:"Dot" }, { n:0x1962, t:"d" }, { n:0x2762, t:"df" },{ n:0x01, t:"%u25BADMS" }, { n:0x02, t:"%u25BADec" }, { n:0x44, t:"D" }, { n:0x65, t:"Degree" }, { n:0x7C, t:"DependAuto" }, { n:0x7D, t:"DependAsk" }, { n:0xB3, t:"det%28" }, { n:0xB5, t:"dim%28" }, { n:0xE5, t:"DispTable" }, { n:0xA8, t:"DrawInv" }, { n:0xA9, t:"DrawF" }, { n:0xDB, t:"DS%3C%28" }, { n:0xDE, t:"Disp" }, { n:0xDF, t:"DispGraph" }, { n:0x54BB, t:"DelVar" }, { n:0x66BB, t:"DiagnosticOn" }, { n:0x07BB, t:"dbd%28" }, { n:0x67BB, t:"DiagnosticOff" }, { n:0xB3BB, t:"d" }, { n:0x06EF, t:"dayOfWk%28" }, { n:0x6AEF, t:"DetectAsymOn" }, { n:0x3BEF, t:"DEC" }, { n:0x6BEF, t:"DetectAsymOff" }, { n:0x4FEF, t:"DARKGREY" }, { n:0x75EF, t:"Dot-Thin" }, ],
E [ { n:0x1A62, t:"e" }, { n:0x3A62, t:"Error df" }, { n:0x3B62, t:"Error SS" }, { n:0x3C62, t:"Error MS" },{ n:0x06BB, t:"%u25BAEff%28" }, { n:0x45, t:"E" }, { n:0x68, t:"Eng" }, { n:0x3B, t:"E" }, { n:0xD0, t:"Else" }, { n:0xD4, t:"End" }, { n:0xF5, t:"ExpReg" }, { n:0xBF, t:"e%5E%28" }, { n:0x50BB, t:"ExprOn" }, { n:0x31BB, t:"e" }, { n:0x51BB, t:"ExprOff" }, { n:0x55BB, t:"Equ%u25BAString%28" }, { n:0x2ABB, t:"expr%28" }, { n:0xB4BB, t:"e" }, { n:0x12EF, t:"ExecLib" }, { n:0x98EF, t:"eval%28" }, { n:0x9EEF, t:"Execute%20Program" }, ],
F [ {n:0x0061, t:"GDB1" }, {n:0x0161, t:"GDB2" }, {n:0x0261, t:"GDB3" }, {n:0x0361, t:"GDB4" }, {n:0x0461, t:"GDB5" }, {n:0x0561, t:"GDB6" }, {n:0x0661, t:"GDB7" }, {n:0x0761, t:"GDB8" }, {n:0x0861, t:"GDB9" }, {n:0x0961, t:"GDB0" }, {n:0x0A7E, t:"GridOn" }, {n:0x0B7E, t:"GridOff" },{n:0x2F63, t:"FV" }, { n:0x2662, t:"F" }, { n:0x3762, t:"Factor df" }, { n:0x3862, t:"Factor SS" }, { n:0x3962, t:"Factor MS" },{ n:0x03, t:"%u25BAFrac" }, { n:0x73, t:"Fix" }, { n:0x24, t:"fnInt%28" }, { n:0x75, t:"Full" }, { n:0x46, t:"F" }, { n:0x76, t:"Func" }, { n:0x27, t:"fMin%28" }, { n:0x28, t:"fMax%28" }, { n:0x69, t:"Float" }, { n:0xE2, t:"Fill%28" }, { n:0xD3, t:"For%28" }, { n:0x96, t:"FnOn" }, { n:0x97, t:"FnOff" }, { n:0xBA, t:"fPart%28" }, { n:0x14BB, t:"Fcdf%28" }, { n:0x1EBB, t:"Fpdf%28" }, { n:0xB5BB, t:"f" }, { n:0xAFBB, t:"F" }, { n:0x3CEF, t:"FRAC" }, { n:0x3DEF, t:"FRAC-APPROX" }, ],
G [ { n:0x47, t:"G" }, { n:0xD7, t:"Goto" }, { n:0xE8, t:"Get%28" }, { n:0xAD, t:"getKey" }, { n:0x53BB, t:"GetCalc%28" }, { n:0x64BB, t:"G-T" }, { n:0x45BB, t:"GraphStyle%28" }, { n:0x09BB, t:"gcd%28" }, { n:0x19BB, t:"geometpdf%28" }, { n:0x1ABB, t:"geometcdf%28" }, { n:0xB6BB, t:"g" }, { n:0xCEBB, t:"GarbageCollect" }, { n:0x45EF, t:"GREEN" }, { n:0x65EF, t:"GraphColor%28" }, { n:0x07EF, t:"getDtStr" }, { n:0x08EF, t:"getTmStr%28" }, { n:0x09EF, t:"getDate" }, { n:0x0AEF, t:"getTime" }, { n:0x5AEF, t:"Gridline" }, { n:0x0CEF, t:"getDtFmt" }, { n:0x0DEF, t:"getTmFmt" }, { n:0x4EEF, t:"GREY" }, ],
H [ { n:0x74, t:"Horiz" }, { n:0x48, t:"H" }, { n:0xA6, t:"Horizontal" }, { n:0xFC, t:"Histogram" }, { n:0xB7BB, t:"h" }, ],
I [ {n:0x2C63, t:"I%" },{ n:0x04BB, t:"%u03A3Int%28" }, { n:0x49, t:"I" }, { n:0x7A, t:"IndpntAuto" }, { n:0x7B, t:"IndpntAsk" }, { n:0x2C, t:"i" }, { n:0xB1, t:"int%28" }, { n:0xB4, t:"identity%28" }, { n:0xB9, t:"iPart%28" }, { n:0xDA, t:"IS%3E%28" }, { n:0xDC, t:"Input" }, { n:0xCE, t:"If" }, { n:0x01BB, t:"irr%28" }, { n:0x11BB, t:"invNorm%28" }, { n:0x27BB, t:"imag%28" }, { n:0x0FBB, t:"inString%28" }, { n:0xB8BB, t:"i" }, { n:0x50EF, t:"Image1" }, { n:0x51EF, t:"Image2" }, { n:0x52EF, t:"Image3" }, { n:0x13EF, t:"invT%28" }, { n:0x53EF, t:"Image4" }, { n:0x54EF, t:"Image5" }, { n:0x55EF, t:"Image6" }, { n:0x56EF, t:"Image7" }, { n:0x57EF, t:"Image8" }, { n:0x58EF, t:"Image9" }, { n:0x59EF, t:"Image0" }, { n:0x0EEF, t:"isClockOn" }, { n:0xA0EF, t:"Insert%20Line%20Above" }, { n:0xA4EF, t:"Insert%20Comment%20Above" }, { n:0x95EF, t:"invBinom%28" }, ],
J [ { n:0x4A, t:"J" }, { n:0xB9BB, t:"j" }, ],
K [ { n:0x4B, t:"K" }, { n:0xBABB, t:"k" }, ],
L [ {n:0x005D, t:"L1" }, {n:0x015D, t:"L2" }, {n:0x025D, t:"L3" }, {n:0x035D, t:"L4" }, {n:0x045D, t:"L5" }, {n:0x055D, t:"L6" }, {n:0x0C7E, t:"LabelOn" }, {n:0x0D7E, t:"LabelOff" }, { n:0x3262, t:"lower" },{ n:0x2CBB, t:"%u0394List%28" }, { n:0x4C, t:"L" }, { n:0xC0, t:"log%28" }, { n:0xF4, t:"LinReg%28a+bx%29" }, { n:0xD6, t:"Lbl" }, { n:0xF6, t:"LnReg" }, { n:0x9C, t:"Line%28" }, { n:0xBE, t:"ln%28" }, { n:0xFF, t:"LinReg%28ax+b%29" }, { n:0x33BB, t:"Logistic" }, { n:0x34BB, t:"LinRegTTest" }, { n:0x08BB, t:"lcm%28" }, { n:0x3ABB, t:"List%u25BAmatr%28" }, { n:0x2BBB, t:"length%28" }, { n:0xBCBB, t:"l" }, { n:0x34EF, t:"logBASE%28" }, { n:0x15EF, t:"LinRegTInt" }, { n:0x49EF, t:"LTBLUE" }, { n:0x4CEF, t:"LTGREY" }, { n:0x92EF, t:"LEFT" }, ],
M [ { n:0x1362, t:"Med" },{ n:0x0862, t:"minX" }, { n:0x0962, t:"maxX" }, { n:0x0A62, t:"minY" }, { n:0x0B62, t:"maxY" },{ n:0x21, t:"mean%28" }, { n:0x19, t:"max%28" }, { n:0x1A, t:"min%28" }, { n:0x4D, t:"M" }, { n:0x1F, t:"median%28" }, { n:0xE6, t:"Menu%28" }, { n:0xF8, t:"Med-Med" }, { n:0x39BB, t:"Matr%u25BAlist%28" }, { n:0x5ABB, t:"ModBoxplot" }, { n:0xBDBB, t:"m" }, { n:0x44EF, t:"MAGENTA" }, { n:0x16EF, t:"Manual-Fit" }, { n:0x36EF, t:"MATHPRINT" }, { n:0x4DEF, t:"MEDGREY" }, { n:0x1EEF, t:"mathprintbox" }, ],
N [ {n:0x1D63, t:"nMax" }, {n:0x1F63, t:"nMin" }, {n:0x2B63, t:"N" }, { n:0x0262, t:"n" }, { n:0x2162, t:"n" }, { n:0x2D62, t:"n1" }, { n:0x3062, t:"n2" },{ n:0x05BB, t:"%u25BANom%28" }, { n:0x25, t:"nDeriv%28" }, { n:0x66, t:"Normal" }, { n:0x4E, t:"N" }, { n:0x3F, t:"newline" }, { n:0x94, t:"nPr" }, { n:0x95, t:"nCr" }, { n:0xB8, t:"not%28" }, { n:0x00BB, t:"npv%28" }, { n:0x10BB, t:"normalcdf%28" }, { n:0x1BBB, t:"normalpdf%28" }, { n:0x5BBB, t:"NormProbPlot" }, { n:0xBEBB, t:"n" }, { n:0x38EF, t:"n/d" }, { n:0x48EF, t:"NAVY" },  ],
O [ { n:0x3C, t:"or" }, { n:0x4F, t:"O" }, { n:0xE0, t:"Output%28" }, { n:0xBFBB, t:"o" }, { n:0x11EF, t:"OpenLib%28" }, { n:0x46EF, t:"ORANGE" }, ],
P [ {n:0x3463, t:"PlotStep" }, {n:0x1B63, t:"PlotStart" }, {n:0x2D63, t:"PV" }, {n:0x2E63, t:"PMT" }, {n:0x027E, t:"PolarGC" }, {n:0x3063, t:"P/Y" }, { n:0x2262, t:"p" }, { n:0x2862, t:"p^" }, { n:0x2962, t:"p^1" }, { n:0x2A62, t:"p^2" }, {n:0x0060, t:"Pic1" }, {n:0x0160, t:"Pic2" }, {n:0x0260, t:"Pic3" }, {n:0x0360, t:"Pic4" }, {n:0x0460, t:"Pic5" }, {n:0x0560, t:"Pic6" }, {n:0x0660, t:"Pic7" }, {n:0x0760, t:"Pic8" }, {n:0x0860, t:"Pic9" }, {n:0x0960, t:"Pic0" },{ n:0x30BB, t:"%u25BAPolar" }, { n:0x03BB, t:"%u03A3prn%28" }, { n:0x44BB, t:"2-PropZInt%28" }, { n:0x43BB, t:"1-PropZInt%28" }, { n:0x3EBB, t:"1-PropZTest%28" }, { n:0x3FBB, t:"2-PropZTest%28" }, { n:0x50, t:"P" }, { n:0x13, t:"pxl-Test%28" }, { n:0x77, t:"Param" }, { n:0x78, t:"Polar" }, { n:0x1D, t:"P%u25BARx%28" }, { n:0x1E, t:"P%u25BARy" }, { n:0x5F, t:"prgm" }, { n:0xA0, t:"Pt-Change%28" }, { n:0x91, t:"PrintScreen" }, { n:0xA1, t:"Pxl-On%28" }, { n:0xA2, t:"Pxl-Off%28" }, { n:0xA3, t:"Pxl-Change%28" }, { n:0xB7, t:"prod%28" }, { n:0xF7, t:"PwrReg" }, { n:0xD8, t:"Pause" }, { n:0xE9, t:"PlotsOn" }, { n:0xEA, t:"PlotsOff" }, { n:0xEC, t:"Plot1%28" }, { n:0xDD, t:"Prompt" }, { n:0xED, t:"Plot2%28" }, { n:0x9E, t:"Pt-On%28" }, { n:0xEE, t:"Plot3%28" }, { n:0x9F, t:"Pt-Off%28" }, { n:0x17BB, t:"poissonpdf%28" }, { n:0x18BB, t:"poissoncdf%28" }, { n:0x4BBB, t:"Pmt_End" }, { n:0x4CBB, t:"Pmt_Bgn" }, { n:0xC0BB, t:"p" }, { n:0xADBB, t:"p%0A%5E" }, { n:0xA3EF, t:"Paste%20Line%20Below" }, { n:0xA6EF, t:"piecewise%28" }, { n:0x79EF, t:"PlySmlt2" }, ],
Q [ { n:0x1462, t:"Q1" }, { n:0x1562, t:"Q3" },{ n:0x51, t:"Q" }, { n:0x2F, t:"QuartReg" }, { n:0xF9, t:"QuadReg" }, { n:0xC1BB, t:"q" }, { n:0x81EF, t:"Quartiles%20Setting%u2026" }, { n:0xA5EF, t:"Quit%20Editor" }, ],,
R [ { n:0x0162, t:"RegEq" }, {n:0x037E, t:"RectGC" }, { n:0x1262, t:"r" }, { n:0x3562, t:"r2" }, { n:0x3662, t:"R2" }, {n:0x405E, t:"r1" }, {n:0x415E, t:"r2" }, {n:0x425E, t:"r3" }, {n:0x435E, t:"r4" }, {n:0x445E, t:"r5" }, {n:0x455E, t:"r6" },{ n:0x2FBB, t:"%u25BARect" }, { n:0x18, t:"*row+%28" }, { n:0x17, t:"*row%28" },{ n:0x20, t:"randM%28" }, { n:0x12, t:"round%28" }, { n:0x52, t:"R" }, { n:0x64, t:"Radian" }, { n:0x15, t:"rowSwap%28" }, { n:0x16, t:"row+%28" }, { n:0x0A, t:"r" }, { n:0x1B, t:"R%u25BAPr%28" }, { n:0x1C, t:"R%u25BAP%u03B8%28" }, { n:0xD2, t:"Repeat" }, { n:0xD5, t:"Return" }, { n:0x99, t:"RecallPic" }, { n:0x9B, t:"RecallGDB" }, { n:0xAB, t:"rand" }, { n:0x26BB, t:"real%28" }, { n:0x0ABB, t:"randInt%28" }, { n:0x0BBB, t:"randBin%28" }, { n:0x2DBB, t:"ref%28" }, { n:0x4DBB, t:"Real" }, { n:0x2EBB, t:"rref%28" }, { n:0x4EBB, t:"re%5E%u03B8i" }, { n:0x1FBB, t:"randNorm%28" }, { n:0xD0BB, t:"reserved" }, { n:0xC2BB, t:"r" }, { n:0x32EF, t:"remainder%28" }, { n:0x42EF, t:"RED" }, { n:0x35EF, t:"randIntNoRep%28" }, { n:0x94EF, t:"RIGHT" }, ],
S [ {n:0x007E, t:"Sequential" }, {n:0x017E, t:"Simul" }, { n:0x2C62, t:"Sx1" }, { n:0x3462, t:"s" }, { n:0x3162, t:"Sxp" }, { n:0x2F62, t:"Sx2" }, {n:0x00AA, t:"Str1" }, {n:0x01AA, t:"Str2" }, {n:0x02AA, t:"Str3" }, {n:0x03AA, t:"Str4" }, {n:0x04AA, t:"Str5" }, {n:0x05AA, t:"Str6" }, {n:0x06AA, t:"Str7" }, {n:0x07AA, t:"Str8" }, {n:0x08AA, t:"Str9" }, {n:0x09AA, t:"Str0" },{ n:0x42BB, t:"2-SampZInt%28" }, { n:0x46BB, t:"2-SampTTest" }, { n:0x47BB, t:"2-SampFTest" }, { n:0x49BB, t:"2-SampTInt" }, { n:0x3DBB, t:"2-SampZTest%28" }, { n:0x22, t:"solve%28" }, { n:0x23, t:"seq%28" }, { n:0x53, t:"S" }, { n:0x67, t:"Sci" }, { n:0x79, t:"Seq" }, { n:0xC2, t:"sin%28" }, { n:0xC3, t:"sin%u05BF%B9%28" }, { n:0xE3, t:"SortA%28" }, { n:0xA4, t:"Shade%28" }, { n:0xE4, t:"SortD%28" }, { n:0xB6, t:"sum%28" }, { n:0xE7, t:"Send%28" }, { n:0x98, t:"StorePic" }, { n:0xC8, t:"sinh%28" }, { n:0xC9, t:"sinh%u05BF%B9%28" }, { n:0xD9, t:"Stop" }, { n:0x9A, t:"StoreGDB" }, { n:0xFE, t:"Scatter" }, { n:0x32BB, t:"SinReg" }, { n:0x35BB, t:"ShadeNorm%28" }, { n:0x36BB, t:"Shade_t%28" }, { n:0x56BB, t:"String%u25BAEqu%28" }, { n:0x37BB, t:"Shade%u03C7%B2" }, { n:0x38BB, t:"ShadeF%28" }, { n:0x58BB, t:"Select%28" }, { n:0x4ABB, t:"SetUpEditor" }, { n:0x0CBB, t:"sub%28" }, { n:0x0DBB, t:"stdDev%28" }, { n:0xC3BB, t:"s" }, { n:0x00EF, t:"setDate%28" }, { n:0x01EF, t:"setTime%28" }, { n:0x03EF, t:"setDtFmt%28" }, { n:0x04EF, t:"setTmFmt%28" }, { n:0x0BEF, t:"startTmr" }, { n:0x90EF, t:"SEQ%28n+1%29" }, { n:0x91EF, t:"SEQ%28n+2%29" }, { n:0x8FEF, t:"SEQ%28n%29" }, ],
T [ {n:0x2A63, t:"TblInput" }, {n:0x1A63, t:"TblStart" }, {n:0x3863, t:"TraceStep" }, {n:0x0F7E, t:"Time" }, {n:0x2163, t:"ΔTbl" }, {n:0x2263, t:"Tstep" }, {n:0x0E63, t:"Tmin" }, {n:0x0F63, t:"Tmax" }, { n:0x2462, t:"t" },{ n:0x40BB, t:"%u03C7%B2-Test%28" }, { n:0x54, t:"T" }, { n:0x0E, t:"T" }, { n:0x93, t:"Text%28" }, { n:0x84, t:"Trace" }, { n:0xC6, t:"tan%28" }, { n:0xA7, t:"Tangent%28" }, { n:0xC7, t:"tan%u05BF%B9%28" }, { n:0xCC, t:"tanh%28" }, { n:0xCD, t:"tanh%u05BF%B9%28" }, { n:0xCF, t:"Then" }, { n:0xEF, t:"TI-84+%28C%28S%29E%29" }, { n:0x20BB, t:"tvm_Pmt" }, { n:0x21BB, t:"tvm_I%25" }, { n:0x12BB, t:"tcdf%28" }, { n:0x22BB, t:"tvm_PV" }, { n:0x23BB, t:"tvm_N" }, { n:0x24BB, t:"tvm_FV" }, { n:0x48BB, t:"TInterval" }, { n:0x1CBB, t:"tpdf%28" }, { n:0x3CBB, t:"T-Test" }, { n:0xC4BB, t:"t" }, { n:0xDFBB, t:"T" }, { n:0x05EF, t:"timeCnv%28" }, { n:0x67EF, t:"TextColor%28" }, { n:0x73EF, t:"tinydotplot" }, { n:0x74EF, t:"Thin" }, { n:0x97EF, t:"toString%28" }, ],
U [ {n:0x0463, t:"u(nMin)" }, {n:0x0663, t:"u(n-1)" }, {n:0x107E, t:"uvAxes" }, {n:0x127E, t:"uwAxes" }, { n:0x3362, t:"upper" }, {n:0x805E, t:"u" },{ n:0x55, t:"U" }, { n:0xC5BB, t:"u" }, { n:0x69BB, t:"UnArchive" },,,,, { n:0x39EF, t:"Un/d" }, { n:0x82EF, t:"u%28n-2%29" }, { n:0x85EF, t:"u%28n-1%29" }, { n:0x88EF, t:"u%28n%29" }, { n:0x8BEF, t:"u%28n+1%29" }, { n:0x9FEF, t:"Undo%20Clear" }, ],
V [ {n:0x0563, t:"v(nMin)" }, {n:0x0763, t:"v(n-1)" }, {n:0x117E, t:"vwAxes" }, {n:0x815E, t:"v" },{ n:0xF3, t:"2-Var%20Stats" }, { n:0xF2, t:"1-Var%20Stats" }, { n:0x56, t:"V" }, { n:0x9D, t:"Vertical" }, { n:0x0EBB, t:"variance%28" }, { n:0xC6BB, t:"v" }, { n:0x83EF, t:"v%28n-2%29" }, { n:0x86EF, t:"v%28n-1%29" }, { n:0x89EF, t:"v%28n%29" }, { n:0x8CEF, t:"v%28n+1%29" }, ],
W [ {n:0x3263, t:"w(nMin)" }, {n:0x0E7E, t:"Web" }, {n:0x825E, t:"w" },{ n:0x57, t:"W" }, { n:0xD1, t:"While" }, { n:0xC7BB, t:"w" }, { n:0x4BEF, t:"WHITE" }, { n:0x84EF, t:"w%28n-2%29" }, { n:0x96EF, t:"Wait" }, { n:0x87EF, t:"w%28n-1%29" }, { n:0x8AEF, t:"w%28n%29" }, { n:0x8DEF, t:"w%28n+1%29" }, ],
X [ {n:0x0263, t:"Xscl" }, {n:0x2863, t:"XFact" }, {n:0x0A63, t:"Xmin" }, {n:0x0B63, t:"Xmax" }, {n:0x2663, t:"ΔX" }, {n:0x3663, t:"Xres" }, { n:0x0362, t:"x¯¯¯" }, { n:0x0462, t:"Σx" }, { n:0x0562, t:"Σx²" }, { n:0x0662, t:"Sx" }, { n:0x0762, t:"σx" }, { n:0x1162, t:"Σxy" }, { n:0x1B62, t:"x1" }, { n:0x1C62, t:"x2" }, { n:0x1D62, t:"x3" }, { n:0x2562, t:"χ²" }, { n:0x2B62, t:"x¯¯¯1" }, { n:0x2E62, t:"x¯¯¯2" }, {n:0x205E, t:"X1T" }, {n:0x225E, t:"X2T" }, {n:0x245E, t:"X3T" }, {n:0x265E, t:"X4T" }, {n:0x285E, t:"X5T" }, {n:0x2A5E, t:"X6T" },{ n:0x13BB, t:"%u03C7%B2cdf%28" }, { n:0x1DBB, t:"%u03C7%B2pdf%28" }, { n:0x58, t:"X" }, { n:0x3D, t:"xor" }, { n:0xFD, t:"xyLine" }, { n:0xF0BB, t:"x" }, { n:0xC8BB, t:"x" }, { n:0xDEBB, t:"x" }, ],
Y [ {n:0x0363, t:"Yscl" }, {n:0x0C63, t:"Ymin" }, {n:0x0D63, t:"Ymax" }, {n:0x2763, t:"ΔY" }, {n:0x2963, t:"YFact" }, { n:0x0C62, t:"y¯¯¯" }, { n:0x0D62, t:"Σy" }, { n:0x0E62, t:"Σy²" }, { n:0x0F62, t:"Sy" }, { n:0x1062, t:"σy" }, { n:0x1E62, t:"y1" }, { n:0x1F62, t:"y2" }, { n:0x2062, t:"y3" }, {n:0x105E, t:"Y1" }, {n:0x115E, t:"Y2" }, {n:0x125E, t:"Y3" }, {n:0x135E, t:"Y4" }, {n:0x145E, t:"Y5" }, {n:0x155E, t:"Y6" }, {n:0x165E, t:"Y7" }, {n:0x175E, t:"Y8" }, {n:0x185E, t:"Y9" }, {n:0x195E, t:"Y0" }, {n:0x215E, t:"Y1T" }, {n:0x235E, t:"Y2T" }, {n:0x255E, t:"Y3T" }, {n:0x275E, t:"Y4T" }, {n:0x295E, t:"Y5T" }, {n:0x2B5E, t:"Y6T" },{ n:0x59, t:"Y" }, { n:0xC9BB, t:"y" }, { n:0x4AEF, t:"YELLOW" }, ],
Z [ {n:0x0863, t:"Zu(nMin)" }, {n:0x0963, t:"Zv(nMin)" }, {n:0x1C63, t:"ZPlotStart" }, {n:0x2463, t:"ZTstep" }, {n:0x2563, t:"Zθstep" }, {n:0x1E63, t:"ZnMax" }, {n:0x3563, t:"ZPlotStep" }, {n:0x1263, t:"ZXmin" }, {n:0x1363, t:"ZXmax" }, {n:0x1463, t:"ZYmin" }, {n:0x1563, t:"ZYmax" }, {n:0x1663, t:"Zθmin" }, {n:0x1763, t:"Zθmax" }, {n:0x1863, t:"ZTmin" }, {n:0x1963, t:"ZTmax" }, {n:0x0063, t:"ZXscl" }, {n:0x0163, t:"ZYscl" }, {n:0x2063, t:"ZnMin" }, {n:0x3363, t:"Zw(nMin)" }, {n:0x3763, t:"ZXres" }, { n:0x2362, t:"z" },{ n:0x5A, t:"Z" }, { n:0x90, t:"ZoomRcl" }, { n:0x92, t:"ZoomSto" }, { n:0x86, t:"ZStandard" }, { n:0x87, t:"ZTrig" }, { n:0x88, t:"ZBox" }, { n:0x89, t:"Zoom%20In" }, { n:0x8A, t:"Zoom%20Out" }, { n:0x8B, t:"ZSquare" }, { n:0x8C, t:"ZInteger" }, { n:0x8D, t:"ZPrevious" }, { n:0x8E, t:"ZDecimal" }, { n:0x8F, t:"ZoomStat" }, { n:0x41BB, t:"ZInterval" }, { n:0x65BB, t:"ZoomFit" }, { n:0x3BBB, t:"Z-Test%28" }, { n:0xCABB, t:"z" }, { n:0x17EF, t:"ZQuadrant1" }, { n:0x18EF, t:"ZFrac1/2" }, { n:0x19EF, t:"ZFrac1/3" }, { n:0x1AEF, t:"ZFrac1/4" }, { n:0x1BEF, t:"ZFrac1/5" }, { n:0x1CEF, t:"ZFrac1/8" }, { n:0x1DEF, t:"ZFrac1/10" }, ],
  [ {n:0x1063, t:"θmin" }, {n:0x1163, t:"θmax" }, {n:0x2363, t:"θstep" }, {n:0x005C, t:"[A]" }, {n:0x015C, t:"[B]" }, {n:0x025C, t:"[C]" }, {n:0x035C, t:"[D]" }, {n:0x045C, t:"[E]" }, {n:0x055C, t:"[F]" }, {n:0x065C, t:"[G]" }, {n:0x075C, t:"[H]" }, {n:0x085C, t:"[I]" }, {n:0x095C, t:"[J]" },{ n:0x0062, "t":"?" }, { n:0x10, t:"%28" }, { n:0x30, t:"0" }, { n:0x70, t:"+" }, { n:0x11, t:"%29" }, { n:0x31, t:"1" }, { n:0x71, t:"-%20%28sub.%29" }, { n:0x32, t:"2" }, { n:0x33, t:"3" }, { n:0x04, t:"%u2192" }, { n:0x34, t:"4" }, { n:0x35, t:"5" }, { n:0x06, t:"%5B" }, { n:0x36, t:"6" }, { n:0x07, t:"%5D" }, { n:0x37, t:"7" }, { n:0x08, t:"%7B" }, { n:0x38, t:"8" }, { n:0x09, t:"%7D" }, { n:0x29, t:" " }, { n:0x39, t:"9" }, { n:0x2A, t:"%22" }, { n:0x3A, t:"." }, { n:0x6A, t:"%3D" }, { n:0x0B, t:"%B0" }, { n:0x2B, t:"%2C" }, { n:0x5B, t:"%u03B8" }, { n:0x6B, t:"%3C" }, { n:0x0C, t:"%u05BF%B9" }, { n:0x6C, t:"%3E" }, { n:0x0D, t:"%B2" }, { n:0x2D, t:"%21" }, { n:0x6D, t:"%u2264" }, { n:0x3E, t:"%3A" }, { n:0x6E, t:"%u2265" }, { n:0x0F, t:"%B3" }, { n:0x6F, t:"%u2260" }, { n:0x7F, t:"%20mark" }, { n:0x80, t:"%20mark" }, { n:0x81, t:"%20mark" }, { n:0xB0, t:"-%20%28neg.%29" }, { n:0xF0, t:"%5E" }, , { n:0xC1, t:"10%5E%28" }, { n:0xF1, t:"%D7%u221A" }, { n:0x82, t:"*" }, { n:0x83, t:"/" }, { n:0xEB, t:"%u221F" }, { n:0xAC, t:"%u03C0" }, { n:0xBC, t:"%u221A%28" }, { n:0xBD, t:"%B3%u221A%28" }, { n:0xAE, t:"%27" }, { n:0xAF, t:"%3F" },{ n:0x70BB, t:"%C2" }, { n:0x80BB, t:"%CE" }, { n:0x90BB, t:"%DB" }, { n:0xA0BB, t:"%u03B2" }, { n:0xE0BB, t:"0" }, { n:0x71BB, t:"%C4" }, { n:0x81BB, t:"%CF" }, { n:0x91BB, t:"%DC" }, { n:0xA1BB, t:"%u03B3" }, { n:0xD1BB, t:"@" }, { n:0xE1BB, t:"1" }, { n:0xF1BB, t:"%u222B" }, { n:0x72BB, t:"%E1" }, { n:0x82BB, t:"%ED" }, { n:0x92BB, t:"%FA" }, { n:0xA2BB, t:"%u0394" }, { n:0xD2BB, t:"%23" }, { n:0xE2BB, t:"2" }, { n:0xF2BB, t:"" }, { n:0x73BB, t:"%E0" }, { n:0x83BB, t:"%EC" }, { n:0x93BB, t:"%F9" }, { n:0xA3BB, t:"%u03B4" }, { n:0xD3BB, t:"%24" }, { n:0xE3BB, t:"3" }, { n:0xF3BB, t:"" }, { n:0x74BB, t:"%E2" }, { n:0x84BB, t:"%EE" }, { n:0x94BB, t:"%FB" }, { n:0xA4BB, t:"%u03B5" }, { n:0xD4BB, t:"%26" }, { n:0xE4BB, t:"4" }, { n:0xF4BB, t:"%u221A" }, { n:0x75BB, t:"%E4" }, { n:0x85BB, t:"%EF" }, { n:0x95BB, t:"%FC" }, { n:0xA5BB, t:"%u03BB" }, { n:0xD5BB, t:"%60" }, { n:0xE5BB, t:"5" }, { n:0xF5BB, t:"" }, { n:0x76BB, t:"%C9" }, { n:0x86BB, t:"%D3" }, { n:0x96BB, t:"%C7" }, { n:0xA6BB, t:"%u03BC" }, { n:0xD6BB, t:"%3B" }, { n:0xE6BB, t:"6" }, { n:0xF6BB, t:"" }, { n:0x77BB, t:"%C8" }, { n:0x87BB, t:"%D2" }, { n:0x97BB, t:"%E7" }, { n:0xA7BB, t:"%u03C0" }, { n:0xD7BB, t:"%5C" }, { n:0xE7BB, t:"7" }, { n:0xF7BB, t:"" }, { n:0x78BB, t:"%CA" }, { n:0x88BB, t:"%D4" }, { n:0x98BB, t:"%D1" }, { n:0xA8BB, t:"%u03C1" }, { n:0xD8BB, t:"%7C" }, { n:0xE8BB, t:"8" }, { n:0xF8BB, t:"" }, { n:0x79BB, t:"%CB" }, { n:0x89BB, t:"%D6" }, { n:0x99BB, t:"%F1" }, { n:0xA9BB, t:"%u03A3" }, { n:0xD9BB, t:"_" }, { n:0xE9BB, t:"9" }, { n:0xF9BB, t:"" }, { n:0x7ABB, t:"%E9" }, { n:0x8ABB, t:"%F3" }, { n:0x9ABB, t:"%B4" }, { n:0xDABB, t:"%25" }, { n:0xEABB, t:"10" }, { n:0xFABB, t:"" }, { n:0x7BBB, t:"%E8" }, { n:0x8BBB, t:"%F2" }, { n:0x9BBB, t:"%60" }, { n:0xABBB, t:"%u03C6" }, { n:0xCBBB, t:"%u03C3" }, { n:0xDBBB, t:"%u2026" }, { n:0xEBBB, t:"%u2190" }, { n:0xFBBB, t:"" }, { n:0x7CBB, t:"%EA" }, { n:0x8CBB, t:"%F4" }, { n:0x9CBB, t:"%A8" }, { n:0xACBB, t:"%u03A9" }, { n:0xCCBB, t:"%u03C4" }, { n:0xDCBB, t:"%u2220" }, { n:0xECBB, t:"%u2192" }, { n:0xFCBB, t:"" }, { n:0x7DBB, t:"%EB" }, { n:0x8DBB, t:"%F6" }, { n:0x9DBB, t:"%BF" }, { n:0xCDBB, t:"%CD" }, { n:0xDDBB, t:"%DF" }, { n:0xEDBB, t:"%u2191" }, { n:0xFDBB, t:"" }, { n:0x6EBB, t:"%C1" }, { n:0x8EBB, t:"%DA" }, { n:0x9EBB, t:"%A1" }, { n:0xAEBB, t:"%u03C7" }, { n:0xEEBB, t:"%u2193" }, { n:0xFEBB, t:"" }, { n:0x6FBB, t:"%C0" }, { n:0x7FBB, t:"%CC" }, { n:0x8FBB, t:"%D9" }, { n:0x9FBB, t:"%u03B1" }, { n:0xCFBB, t:"%7E" }, { n:0xFFBB, t:"" }, ],

// sorter is 
function alpha_value(a) { var r = 0; var len = 0; for(var i = 0 ; i < a.length; ++i) { var tl = a[i].toLowerCase().charCodeAt(0); if(tl >= 'a'.charCodeAt(0) && tl <= 'z'.charCodeAt(0)) { len += 1; } } var m = Math.pow(26, len - 1); for(var i = 0 ; i < a.length; ++i) { var tl = a[i].toLowerCase().charCodeAt(0); if(tl >= 'a'.charCodeAt(0) && tl <= 'z'.charCodeAt(0)) { r += (tl - 'a'.charCodeAt(0))*m; } } return r; }
function sort_func(a,b) { var a_val = alpha_value(a.t); var b_val = alpha_value(b.t); var result; if(a_val < b_val) { result = -1;} else if(a_val > b_val) { result = 1; } else { result = 0; } return result; }
var arrs = 
var str = "";
for(var i in arrs) { arrs[i] = arrs[i].sort(sort_func); str += JSON.stringify(arrs[i]) + "\n"; } console.log(str);
copy(str);

[{"n":5730,"t":"a"},{"n":45243,"t":"a"},{"n":65,"t":"A"},{"n":20411,"t":"a+bi"},{"n":64,"t":"and"},{"n":178,"t":"abs%28"},{"n":27323,"t":"Asm%28"},{"n":114,"t":"Ans"},{"n":15087,"t":"AUTO"},{"n":10427,"t":"angle%28"},{"n":22971,"t":"ANOVA%28"},{"n":2174,"t":"AxesOn"},{"n":26811,"t":"Archive"},{"n":2430,"t":"AxesOff"},{"n":27579,"t":"AsmComp%28"},{"n":20,"t":"augment%28"},{"n":27835,"t":"AsmPrgm"},{"n":26863,"t":"Asm84CPrgm"},{"n":31471,"t":"Asm84CEPrgm"}],
[{"n":5986,"t":"b"},{"n":66,"t":"B"},{"n":45499,"t":"b"},{"n":699,"t":"bal%28"},{"n":16879,"t":"BLUE"},{"n":17391,"t":"BLACK"},{"n":18415,"t":"BROWN"},{"n":5,"t":"Boxplot"},{"n":5819,"t":"binomcdf%28"},{"n":5563,"t":"binompdf%28"},{"n":27887,"t":"BorderColor"},{"n":23535,"t":"BackgroundOn"},{"n":25839,"t":"BackgroundOff"}],
[{"n":6242,"t":"c"},{"n":67,"t":"C"},{"n":45755,"t":"c"},{"n":12643,"t":"C/Y"},{"n":196,"t":"cos%28"},{"n":9659,"t":"conj%28"},{"n":202,"t":"cosh%28"},{"n":165,"t":"Circle%28"},{"n":37871,"t":"CENTER"},{"n":10683,"t":"cumSum%28"},{"n":14319,"t":"CLASSIC"},{"n":197,"t":"cos%u05BF%B9%28"},{"n":4335,"t":"ClockOn"},{"n":225,"t":"ClrHome"},{"n":133,"t":"ClrDraw"},{"n":1150,"t":"CoordOn"},{"n":41455,"t":"Cut%20Line"},{"n":250,"t":"ClrList"},{"n":46,"t":"CubicReg"},{"n":4079,"t":"ClockOff"},{"n":251,"t":"ClrTable"},{"n":203,"t":"cosh%u05BF%B9%28"},{"n":751,"t":"checkTmr%28"},{"n":1406,"t":"CoordOff"},{"n":41711,"t":"Copy%20Line"},{"n":1662,"t":"Connected"},{"n":28091,"t":"compiled%20asm"},{"n":21179,"t":"ClrAllLists"},{"n":22459,"t":"Clear%20Entries"},{"n":31727,"t":"compiled%20asm%20%28CE%29"},{"n":27119,"t":"compiled%20asm%20%28CSE%29"}],
[{"n":6498,"t":"d"},{"n":68,"t":"D"},{"n":46011,"t":"d"},{"n":10082,"t":"df"},{"n":1979,"t":"dbd%28"},{"n":15343,"t":"DEC"},{"n":181,"t":"dim%28"},{"n":219,"t":"DS%3C%28"},{"n":179,"t":"det%28"},{"n":1918,"t":"Dot"},{"n":222,"t":"Disp"},{"n":169,"t":"DrawF"},{"n":2,"t":"%u25BADec"},{"n":101,"t":"Degree"},{"n":1,"t":"%u25BADMS"},{"n":21691,"t":"DelVar"},{"n":1775,"t":"dayOfWk%28"},{"n":30191,"t":"Dot-Thin"},{"n":168,"t":"DrawInv"},{"n":20463,"t":"DARKGREY"},{"n":125,"t":"DependAsk"},{"n":229,"t":"DispTable"},{"n":223,"t":"DispGraph"},{"n":124,"t":"DependAuto"},{"n":26299,"t":"DiagnosticOn"},{"n":27375,"t":"DetectAsymOn"},{"n":26555,"t":"DiagnosticOff"},{"n":27631,"t":"DetectAsymOff"}],
[{"n":6754,"t":"e"},{"n":69,"t":"E"},{"n":59,"t":"E"},{"n":12731,"t":"e"},{"n":46267,"t":"e"},{"n":191,"t":"e%5E%28"},{"n":212,"t":"End"},{"n":104,"t":"Eng"},{"n":39151,"t":"eval%28"},{"n":208,"t":"Else"},{"n":10939,"t":"expr%28"},{"n":1723,"t":"%u25BAEff%28"},{"n":245,"t":"ExpReg"},{"n":20667,"t":"ExprOn"},{"n":4847,"t":"ExecLib"},{"n":14946,"t":"Error df"},{"n":20923,"t":"ExprOff"},{"n":15458,"t":"Error MS"},{"n":15202,"t":"Error SS"},{"n":21947,"t":"Equ%u25BAString%28"},{"n":40687,"t":"Execute%20Program"}],
[{"n":9826,"t":"F"},{"n":70,"t":"F"},{"n":46523,"t":"f"},{"n":44987,"t":"F"},{"n":12131,"t":"FV"},{"n":97,"t":"GDB1"},{"n":353,"t":"GDB2"},{"n":609,"t":"GDB3"},{"n":865,"t":"GDB4"},{"n":1121,"t":"GDB5"},{"n":1377,"t":"GDB6"},{"n":1633,"t":"GDB7"},{"n":1889,"t":"GDB8"},{"n":2145,"t":"GDB9"},{"n":2401,"t":"GDB0"},{"n":115,"t":"Fix"},{"n":211,"t":"For%28"},{"n":5307,"t":"Fcdf%28"},{"n":15599,"t":"FRAC"},{"n":7867,"t":"Fpdf%28"},{"n":226,"t":"Fill%28"},{"n":39,"t":"fMin%28"},{"n":118,"t":"Func"},{"n":40,"t":"fMax%28"},{"n":150,"t":"FnOn"},{"n":117,"t":"Full"},{"n":151,"t":"FnOff"},{"n":105,"t":"Float"},{"n":186,"t":"fPart%28"},{"n":36,"t":"fnInt%28"},{"n":2686,"t":"GridOn"},{"n":3,"t":"%u25BAFrac"},{"n":2942,"t":"GridOff"},{"n":14178,"t":"Factor df"},{"n":14690,"t":"Factor MS"},{"n":14434,"t":"Factor SS"},{"n":15855,"t":"FRAC-APPROX"}],
[{"n":71,"t":"G"},{"n":46779,"t":"g"},{"n":25787,"t":"G-T"},{"n":2491,"t":"gcd%28"},{"n":232,"t":"Get%28"},{"n":20207,"t":"GREY"},{"n":215,"t":"Goto"},{"n":17903,"t":"GREEN"},{"n":173,"t":"getKey"},{"n":21435,"t":"GetCalc%28"},{"n":2543,"t":"getDate"},{"n":2799,"t":"getTime"},{"n":23279,"t":"Gridline"},{"n":3311,"t":"getDtFmt"},{"n":3567,"t":"getTmFmt"},{"n":2031,"t":"getDtStr"},{"n":2287,"t":"getTmStr%28"},{"n":6843,"t":"geometcdf%28"},{"n":6587,"t":"geometpdf%28"},{"n":26095,"t":"GraphColor%28"},{"n":17851,"t":"GraphStyle%28"},{"n":52923,"t":"GarbageCollect"}],
[{"n":72,"t":"H"},{"n":47035,"t":"h"},{"n":116,"t":"Horiz"},{"n":252,"t":"Histogram"},{"n":166,"t":"Horizontal"}],
[{"n":11363,"t":"I%"},{"n":73,"t":"I"},{"n":44,"t":"i"},{"n":47291,"t":"i"},{"n":206,"t":"If"},{"n":218,"t":"IS%3E%28"},{"n":177,"t":"int%28"},{"n":443,"t":"irr%28"},{"n":10171,"t":"imag%28"},{"n":5103,"t":"invT%28"},{"n":20719,"t":"Image1"},{"n":20975,"t":"Image2"},{"n":21231,"t":"Image3"},{"n":21487,"t":"Image4"},{"n":21743,"t":"Image5"},{"n":21999,"t":"Image6"},{"n":22255,"t":"Image7"},{"n":22511,"t":"Image8"},{"n":22767,"t":"Image9"},{"n":23023,"t":"Image0"},{"n":185,"t":"iPart%28"},{"n":1211,"t":"%u03A3Int%28"},{"n":220,"t":"Input"},{"n":4539,"t":"invNorm%28"},{"n":38383,"t":"invBinom%28"},{"n":180,"t":"identity%28"},{"n":4027,"t":"inString%28"},{"n":3823,"t":"isClockOn"},{"n":123,"t":"IndpntAsk"},{"n":122,"t":"IndpntAuto"},{"n":41199,"t":"Insert%20Line%20Above"},{"n":42223,"t":"Insert%20Comment%20Above"}],
[{"n":74,"t":"J"},{"n":47547,"t":"j"}],
[{"n":75,"t":"K"},{"n":47803,"t":"k"}],
[{"n":93,"t":"L1"},{"n":349,"t":"L2"},{"n":605,"t":"L3"},{"n":861,"t":"L4"},{"n":1117,"t":"L5"},{"n":1373,"t":"L6"},{"n":76,"t":"L"},{"n":48315,"t":"l"},{"n":190,"t":"ln%28"},{"n":214,"t":"Lbl"},{"n":2235,"t":"lcm%28"},{"n":192,"t":"log%28"},{"n":156,"t":"Line%28"},{"n":37615,"t":"LEFT"},{"n":246,"t":"LnReg"},{"n":12898,"t":"lower"},{"n":11451,"t":"%u0394List%28"},{"n":11195,"t":"length%28"},{"n":18927,"t":"LTBLUE"},{"n":19695,"t":"LTGREY"},{"n":3198,"t":"LabelOn"},{"n":13551,"t":"logBASE%28"},{"n":3454,"t":"LabelOff"},{"n":13243,"t":"Logistic"},{"n":244,"t":"LinReg%28a+bx%29"},{"n":255,"t":"LinReg%28ax+b%29"},{"n":5615,"t":"LinRegTInt"},{"n":15035,"t":"List%u25BAmatr%28"},{"n":13499,"t":"LinRegTTest"}],
[{"n":77,"t":"M"},{"n":48571,"t":"m"},{"n":4962,"t":"Med"},{"n":26,"t":"min%28"},{"n":25,"t":"max%28"},{"n":33,"t":"mean%28"},{"n":230,"t":"Menu%28"},{"n":2146,"t":"minX"},{"n":2658,"t":"minY"},{"n":2402,"t":"maxX"},{"n":2914,"t":"maxY"},{"n":248,"t":"Med-Med"},{"n":31,"t":"median%28"},{"n":17647,"t":"MAGENTA"},{"n":19951,"t":"MEDGREY"},{"n":5871,"t":"Manual-Fit"},{"n":14063,"t":"MATHPRINT"},{"n":23227,"t":"ModBoxplot"},{"n":14779,"t":"Matr%u25BAlist%28"},{"n":7919,"t":"mathprintbox"}],
[{"n":11107,"t":"N"},{"n":610,"t":"n"},{"n":8546,"t":"n"},{"n":11618,"t":"n1"},{"n":12386,"t":"n2"},{"n":78,"t":"N"},{"n":48827,"t":"n"},{"n":14575,"t":"n/d"},{"n":149,"t":"nCr"},{"n":148,"t":"nPr"},{"n":184,"t":"not%28"},{"n":187,"t":"npv%28"},{"n":8035,"t":"nMin"},{"n":7523,"t":"nMax"},{"n":18671,"t":"NAVY"},{"n":1467,"t":"%u25BANom%28"},{"n":37,"t":"nDeriv%28"},{"n":102,"t":"Normal"},{"n":63,"t":"newline"},{"n":4283,"t":"normalcdf%28"},{"n":7099,"t":"normalpdf%28"},{"n":23483,"t":"NormProbPlot"}],
[{"n":79,"t":"O"},{"n":49083,"t":"o"},{"n":60,"t":"or"},{"n":18159,"t":"ORANGE"},{"n":224,"t":"Output%28"},{"n":4591,"t":"OpenLib%28"}],
[{"n":8802,"t":"p"},{"n":10338,"t":"p^"},{"n":10594,"t":"p^1"},{"n":10850,"t":"p^2"},{"n":80,"t":"P"},{"n":49339,"t":"p"},{"n":11619,"t":"PV"},{"n":12387,"t":"P/Y"},{"n":44475,"t":"p%0A%5E"},{"n":96,"t":"Pic1"},{"n":352,"t":"Pic2"},{"n":608,"t":"Pic3"},{"n":864,"t":"Pic4"},{"n":1120,"t":"Pic5"},{"n":1376,"t":"Pic6"},{"n":1632,"t":"Pic7"},{"n":1888,"t":"Pic8"},{"n":2144,"t":"Pic9"},{"n":2400,"t":"Pic0"},{"n":11875,"t":"PMT"},{"n":183,"t":"prod%28"},{"n":95,"t":"prgm"},{"n":236,"t":"Plot1%28"},{"n":237,"t":"Plot2%28"},{"n":238,"t":"Plot3%28"},{"n":158,"t":"Pt-On%28"},{"n":119,"t":"Param"},{"n":120,"t":"Polar"},{"n":216,"t":"Pause"},{"n":159,"t":"Pt-Off%28"},{"n":955,"t":"%u03A3prn%28"},{"n":161,"t":"Pxl-On%28"},{"n":19387,"t":"Pmt_End"},{"n":19643,"t":"Pmt_Bgn"},{"n":162,"t":"Pxl-Off%28"},{"n":29,"t":"P%u25BARx%28"},{"n":30,"t":"P%u25BARy"},{"n":247,"t":"PwrReg"},{"n":221,"t":"Prompt"},{"n":638,"t":"PolarGC"},{"n":233,"t":"PlotsOn"},{"n":19,"t":"pxl-Test%28"},{"n":31215,"t":"PlySmlt2"},{"n":160,"t":"Pt-Change%28"},{"n":12475,"t":"%u25BAPolar"},{"n":234,"t":"PlotsOff"},{"n":13411,"t":"PlotStep"},{"n":17595,"t":"2-PropZInt%28"},{"n":17339,"t":"1-PropZInt%28"},{"n":163,"t":"Pxl-Change%28"},{"n":42735,"t":"piecewise%28"},{"n":7011,"t":"PlotStart"},{"n":16059,"t":"1-PropZTest%28"},{"n":16315,"t":"2-PropZTest%28"},{"n":6331,"t":"poissoncdf%28"},{"n":6075,"t":"poissonpdf%28"},{"n":145,"t":"PrintScreen"},{"n":41967,"t":"Paste%20Line%20Below"}],
[{"n":5218,"t":"Q1"},{"n":5474,"t":"Q3"},{"n":81,"t":"Q"},{"n":49595,"t":"q"},{"n":249,"t":"QuadReg"},{"n":47,"t":"QuartReg"},{"n":42479,"t":"Quit%20Editor"},{"n":33263,"t":"Quartiles%20Setting%u2026"}],
[{"n":4706,"t":"r"},{"n":13666,"t":"r2"},{"n":13922,"t":"R2"},{"n":16478,"t":"r1"},{"n":16734,"t":"r2"},{"n":16990,"t":"r3"},{"n":17246,"t":"r4"},{"n":17502,"t":"r5"},{"n":17758,"t":"r6"},{"n":82,"t":"R"},{"n":10,"t":"r"},{"n":49851,"t":"r"},{"n":17135,"t":"RED"},{"n":11707,"t":"ref%28"},{"n":24,"t":"*row+%28"},{"n":23,"t":"*row%28"},{"n":22,"t":"row+%28"},{"n":9915,"t":"real%28"},{"n":19899,"t":"Real"},{"n":171,"t":"rand"},{"n":11963,"t":"rref%28"},{"n":32,"t":"randM%28"},{"n":354,"t":"RegEq"},{"n":38127,"t":"RIGHT"},{"n":18,"t":"round%28"},{"n":100,"t":"Radian"},{"n":894,"t":"RectGC"},{"n":20155,"t":"re%5E%u03B8i"},{"n":210,"t":"Repeat"},{"n":27,"t":"R%u25BAPr%28"},{"n":213,"t":"Return"},{"n":3003,"t":"randBin%28"},{"n":12219,"t":"%u25BARect"},{"n":2747,"t":"randInt%28"},{"n":28,"t":"R%u25BAP%u03B8%28"},{"n":21,"t":"rowSwap%28"},{"n":53435,"t":"reserved"},{"n":8123,"t":"randNorm%28"},{"n":155,"t":"RecallGDB"},{"n":153,"t":"RecallPic"},{"n":13039,"t":"remainder%28"},{"n":13807,"t":"randIntNoRep%28"}],
[{"n":13410,"t":"s"},{"n":83,"t":"S"},{"n":50107,"t":"s"},{"n":11362,"t":"Sx1"},{"n":12130,"t":"Sx2"},{"n":103,"t":"Sci"},{"n":35,"t":"seq%28"},{"n":121,"t":"Seq"},{"n":194,"t":"sin%28"},{"n":3259,"t":"sub%28"},{"n":182,"t":"sum%28"},{"n":170,"t":"Str1"},{"n":426,"t":"Str2"},{"n":682,"t":"Str3"},{"n":938,"t":"Str4"},{"n":1194,"t":"Str5"},{"n":1450,"t":"Str6"},{"n":1706,"t":"Str7"},{"n":1962,"t":"Str8"},{"n":2218,"t":"Str9"},{"n":2474,"t":"Str0"},{"n":12642,"t":"Sxp"},{"n":231,"t":"Send%28"},{"n":200,"t":"sinh%28"},{"n":37103,"t":"SEQ%28n+1%29"},{"n":37359,"t":"SEQ%28n+2%29"},{"n":36847,"t":"SEQ%28n%29"},{"n":217,"t":"Stop"},{"n":164,"t":"Shade%28"},{"n":34,"t":"solve%28"},{"n":227,"t":"SortA%28"},{"n":382,"t":"Simul"},{"n":228,"t":"SortD%28"},{"n":14523,"t":"ShadeF%28"},{"n":14011,"t":"Shade_t%28"},{"n":22715,"t":"Select%28"},{"n":12987,"t":"SinReg"},{"n":3515,"t":"stdDev%28"},{"n":195,"t":"sin%u05BF%B9%28"},{"n":239,"t":"setDate%28"},{"n":254,"t":"Scatter"},{"n":495,"t":"setTime%28"},{"n":14267,"t":"Shade%u03C7%B2"},{"n":201,"t":"sinh%u05BF%B9%28"},{"n":154,"t":"StoreGDB"},{"n":152,"t":"StorePic"},{"n":1007,"t":"setDtFmt%28"},{"n":18875,"t":"2-SampTInt"},{"n":1263,"t":"setTmFmt%28"},{"n":17083,"t":"2-SampZInt%28"},{"n":3055,"t":"startTmr"},{"n":13755,"t":"ShadeNorm%28"},{"n":18363,"t":"2-SampFTest"},{"n":18107,"t":"2-SampTTest"},{"n":15803,"t":"2-SampZTest%28"},{"n":126,"t":"Sequential"},{"n":19131,"t":"SetUpEditor"},{"n":22203,"t":"String%u25BAEqu%28"}],
[{"n":9314,"t":"t"},{"n":84,"t":"T"},{"n":14,"t":"T"},{"n":50363,"t":"t"},{"n":57275,"t":"T"},{"n":8547,"t":"ΔTbl"},{"n":198,"t":"tan%28"},{"n":4795,"t":"tcdf%28"},{"n":204,"t":"tanh%28"},{"n":7355,"t":"tpdf%28"},{"n":3966,"t":"Time"},{"n":207,"t":"Then"},{"n":29935,"t":"Thin"},{"n":3683,"t":"Tmin"},{"n":3939,"t":"Tmax"},{"n":8635,"t":"tvm_I%25"},{"n":147,"t":"Text%28"},{"n":9147,"t":"tvm_N"},{"n":132,"t":"Trace"},{"n":239,"t":"TI-84+%28C%28S%29E%29"},{"n":8803,"t":"Tstep"},{"n":9403,"t":"tvm_FV"},{"n":15547,"t":"T-Test"},{"n":8891,"t":"tvm_PV"},{"n":8379,"t":"tvm_Pmt"},{"n":199,"t":"tan%u05BF%B9%28"},{"n":167,"t":"Tangent%28"},{"n":1519,"t":"timeCnv%28"},{"n":16571,"t":"%u03C7%B2-Test%28"},{"n":205,"t":"tanh%u05BF%B9%28"},{"n":6755,"t":"TblStart"},{"n":10851,"t":"TblInput"},{"n":38895,"t":"toString%28"},{"n":14435,"t":"TraceStep"},{"n":18619,"t":"TInterval"},{"n":26607,"t":"TextColor%28"},{"n":29679,"t":"tinydotplot"}],
[{"n":32862,"t":"u"},{"n":85,"t":"U"},{"n":50619,"t":"u"},{"n":1635,"t":"u(n-1)"},{"n":33519,"t":"u%28n-2%29"},{"n":34287,"t":"u%28n-1%29"},{"n":35055,"t":"u%28n%29"},{"n":35823,"t":"u%28n+1%29"},{"n":14831,"t":"Un/d"},{"n":1123,"t":"u(nMin)"},{"n":13154,"t":"upper"},{"n":4222,"t":"uvAxes"},{"n":4734,"t":"uwAxes"},{"n":40943,"t":"Undo%20Clear"},{"n":27067,"t":"UnArchive"}],
[{"n":33118,"t":"v"},{"n":86,"t":"V"},{"n":50875,"t":"v"},{"n":1891,"t":"v(n-1)"},{"n":33775,"t":"v%28n-2%29"},{"n":34543,"t":"v%28n-1%29"},{"n":35311,"t":"v%28n%29"},{"n":36079,"t":"v%28n+1%29"},{"n":1379,"t":"v(nMin)"},{"n":4478,"t":"vwAxes"},{"n":3771,"t":"variance%28"},{"n":157,"t":"Vertical"},{"n":243,"t":"2-Var%20Stats"},{"n":242,"t":"1-Var%20Stats"}],
[{"n":33374,"t":"w"},{"n":87,"t":"W"},{"n":51131,"t":"w"},{"n":34031,"t":"w%28n-2%29"},{"n":34799,"t":"w%28n-1%29"},{"n":35567,"t":"w%28n%29"},{"n":36335,"t":"w%28n+1%29"},{"n":3710,"t":"Web"},{"n":38639,"t":"Wait"},{"n":209,"t":"While"},{"n":19439,"t":"WHITE"},{"n":12899,"t":"w(nMin)"}],
[{"n":9570,"t":"χ²"},{"n":9827,"t":"ΔX"},{"n":866,"t":"x¯¯¯"},{"n":1122,"t":"Σx"},{"n":1378,"t":"Σx²"},{"n":1890,"t":"σx"},{"n":7010,"t":"x1"},{"n":7266,"t":"x2"},{"n":7522,"t":"x3"},{"n":11106,"t":"x¯¯¯1"},{"n":11874,"t":"x¯¯¯2"},{"n":88,"t":"X"},{"n":61627,"t":"x"},{"n":51387,"t":"x"},{"n":57019,"t":"x"},{"n":1634,"t":"Sx"},{"n":8286,"t":"X1T"},{"n":8798,"t":"X2T"},{"n":9310,"t":"X3T"},{"n":9822,"t":"X4T"},{"n":10334,"t":"X5T"},{"n":10846,"t":"X6T"},{"n":4450,"t":"Σxy"},{"n":61,"t":"xor"},{"n":611,"t":"Xscl"},{"n":2659,"t":"Xmin"},{"n":2915,"t":"Xmax"},{"n":13923,"t":"Xres"},{"n":10339,"t":"XFact"},{"n":5051,"t":"%u03C7%B2cdf%28"},{"n":7611,"t":"%u03C7%B2pdf%28"},{"n":253,"t":"xyLine"}],
[{"n":10083,"t":"ΔY"},{"n":3170,"t":"y¯¯¯"},{"n":3426,"t":"Σy"},{"n":3682,"t":"Σy²"},{"n":4194,"t":"σy"},{"n":7778,"t":"y1"},{"n":8034,"t":"y2"},{"n":8290,"t":"y3"},{"n":4190,"t":"Y1"},{"n":4446,"t":"Y2"},{"n":4702,"t":"Y3"},{"n":4958,"t":"Y4"},{"n":5214,"t":"Y5"},{"n":5470,"t":"Y6"},{"n":5726,"t":"Y7"},{"n":5982,"t":"Y8"},{"n":6238,"t":"Y9"},{"n":6494,"t":"Y0"},{"n":89,"t":"Y"},{"n":51643,"t":"y"},{"n":3938,"t":"Sy"},{"n":8542,"t":"Y1T"},{"n":9054,"t":"Y2T"},{"n":9566,"t":"Y3T"},{"n":10078,"t":"Y4T"},{"n":10590,"t":"Y5T"},{"n":11102,"t":"Y6T"},{"n":867,"t":"Yscl"},{"n":3171,"t":"Ymin"},{"n":3427,"t":"Ymax"},{"n":10595,"t":"YFact"},{"n":19183,"t":"YELLOW"}],
[{"n":9058,"t":"z"},{"n":90,"t":"Z"},{"n":51899,"t":"z"},{"n":5731,"t":"Zθmin"},{"n":5987,"t":"Zθmax"},{"n":136,"t":"ZBox"},{"n":6383,"t":"ZFrac1/2"},{"n":6639,"t":"ZFrac1/3"},{"n":6895,"t":"ZFrac1/4"},{"n":7151,"t":"ZFrac1/5"},{"n":7407,"t":"ZFrac1/8"},{"n":7663,"t":"ZFrac1/10"},{"n":8291,"t":"ZnMin"},{"n":7779,"t":"ZnMax"},{"n":135,"t":"ZTrig"},{"n":6243,"t":"ZTmin"},{"n":6499,"t":"ZTmax"},{"n":99,"t":"ZXscl"},{"n":355,"t":"ZYscl"},{"n":9571,"t":"Zθstep"},{"n":4707,"t":"ZXmin"},{"n":5219,"t":"ZYmin"},{"n":4963,"t":"ZXmax"},{"n":5475,"t":"ZYmax"},{"n":15291,"t":"Z-Test%28"},{"n":14179,"t":"ZXres"},{"n":137,"t":"Zoom%20In"},{"n":2147,"t":"Zu(nMin)"},{"n":2403,"t":"Zv(nMin)"},{"n":13155,"t":"Zw(nMin)"},{"n":9315,"t":"ZTstep"},{"n":144,"t":"ZoomRcl"},{"n":26043,"t":"ZoomFit"},{"n":139,"t":"ZSquare"},{"n":146,"t":"ZoomSto"},{"n":138,"t":"Zoom%20Out"},{"n":142,"t":"ZDecimal"},{"n":140,"t":"ZInteger"},{"n":143,"t":"ZoomStat"},{"n":134,"t":"ZStandard"},{"n":6127,"t":"ZQuadrant1"},{"n":16827,"t":"ZInterval"},{"n":13667,"t":"ZPlotStep"},{"n":141,"t":"ZPrevious"},{"n":7267,"t":"ZPlotStart"}],
[{n:0x1063, t:"θmin" }, {n:0x1163, t:"θmax" }, {n:0x2363, t:"θstep" }, {n:0x005C, t:"[A]" }, {n:0x015C, t:"[B]" }, {n:0x025C, t:"[C]" }, {n:0x035C, t:"[D]" }, {n:0x045C, t:"[E]" }, {n:0x055C, t:"[F]" }, {n:0x065C, t:"[G]" }, {n:0x075C, t:"[H]" }, {n:0x085C, t:"[I]" }, {n:0x095C, t:"[J]" },{ n:0x0062, "t":"?" }, { n:0x10, t:"%28" }, { n:0x30, t:"0" }, { n:0x70, t:"+" }, { n:0x11, t:"%29" }, { n:0x31, t:"1" }, { n:0x71, t:"-%20%28sub.%29" }, { n:0x32, t:"2" }, { n:0x33, t:"3" }, { n:0x04, t:"%u2192" }, { n:0x34, t:"4" }, { n:0x35, t:"5" }, { n:0x06, t:"%5B" }, { n:0x36, t:"6" }, { n:0x07, t:"%5D" }, { n:0x37, t:"7" }, { n:0x08, t:"%7B" }, { n:0x38, t:"8" }, { n:0x09, t:"%7D" }, { n:0x29, t:" " }, { n:0x39, t:"9" }, { n:0x2A, t:"%22" }, { n:0x3A, t:"." }, { n:0x6A, t:"%3D" }, { n:0x0B, t:"%B0" }, { n:0x2B, t:"%2C" }, { n:0x5B, t:"%u03B8" }, { n:0x6B, t:"%3C" }, { n:0x0C, t:"%u05BF%B9" }, { n:0x6C, t:"%3E" }, { n:0x0D, t:"%B2" }, { n:0x2D, t:"%21" }, { n:0x6D, t:"%u2264" }, { n:0x3E, t:"%3A" }, { n:0x6E, t:"%u2265" }, { n:0x0F, t:"%B3" }, { n:0x6F, t:"%u2260" }, { n:0x7F, t:"%20mark" }, { n:0x80, t:"%20mark" }, { n:0x81, t:"%20mark" }, { n:0xB0, t:"-%20%28neg.%29" }, { n:0xF0, t:"%5E" }, , { n:0xC1, t:"10%5E%28" }, { n:0xF1, t:"%D7%u221A" }, { n:0x82, t:"*" }, { n:0x83, t:"/" }, { n:0xEB, t:"%u221F" }, { n:0xAC, t:"%u03C0" }, { n:0xBC, t:"%u221A%28" }, { n:0xBD, t:"%B3%u221A%28" }, { n:0xAE, t:"%27" }, { n:0xAF, t:"%3F" },{ n:0x70BB, t:"%C2" }, { n:0x80BB, t:"%CE" }, { n:0x90BB, t:"%DB" }, { n:0xA0BB, t:"%u03B2" }, { n:0xE0BB, t:"0" }, { n:0x71BB, t:"%C4" }, { n:0x81BB, t:"%CF" }, { n:0x91BB, t:"%DC" }, { n:0xA1BB, t:"%u03B3" }, { n:0xD1BB, t:"@" }, { n:0xE1BB, t:"1" }, { n:0xF1BB, t:"%u222B" }, { n:0x72BB, t:"%E1" }, { n:0x82BB, t:"%ED" }, { n:0x92BB, t:"%FA" }, { n:0xA2BB, t:"%u0394" }, { n:0xD2BB, t:"%23" }, { n:0xE2BB, t:"2" }, { n:0xF2BB, t:"" }, { n:0x73BB, t:"%E0" }, { n:0x83BB, t:"%EC" }, { n:0x93BB, t:"%F9" }, { n:0xA3BB, t:"%u03B4" }, { n:0xD3BB, t:"%24" }, { n:0xE3BB, t:"3" }, { n:0xF3BB, t:"" }, { n:0x74BB, t:"%E2" }, { n:0x84BB, t:"%EE" }, { n:0x94BB, t:"%FB" }, { n:0xA4BB, t:"%u03B5" }, { n:0xD4BB, t:"%26" }, { n:0xE4BB, t:"4" }, { n:0xF4BB, t:"%u221A" }, { n:0x75BB, t:"%E4" }, { n:0x85BB, t:"%EF" }, { n:0x95BB, t:"%FC" }, { n:0xA5BB, t:"%u03BB" }, { n:0xD5BB, t:"%60" }, { n:0xE5BB, t:"5" }, { n:0xF5BB, t:"" }, { n:0x76BB, t:"%C9" }, { n:0x86BB, t:"%D3" }, { n:0x96BB, t:"%C7" }, { n:0xA6BB, t:"%u03BC" }, { n:0xD6BB, t:"%3B" }, { n:0xE6BB, t:"6" }, { n:0xF6BB, t:"" }, { n:0x77BB, t:"%C8" }, { n:0x87BB, t:"%D2" }, { n:0x97BB, t:"%E7" }, { n:0xA7BB, t:"%u03C0" }, { n:0xD7BB, t:"%5C" }, { n:0xE7BB, t:"7" }, { n:0xF7BB, t:"" }, { n:0x78BB, t:"%CA" }, { n:0x88BB, t:"%D4" }, { n:0x98BB, t:"%D1" }, { n:0xA8BB, t:"%u03C1" }, { n:0xD8BB, t:"%7C" }, { n:0xE8BB, t:"8" }, { n:0xF8BB, t:"" }, { n:0x79BB, t:"%CB" }, { n:0x89BB, t:"%D6" }, { n:0x99BB, t:"%F1" }, { n:0xA9BB, t:"%u03A3" }, { n:0xD9BB, t:"_" }, { n:0xE9BB, t:"9" }, { n:0xF9BB, t:"" }, { n:0x7ABB, t:"%E9" }, { n:0x8ABB, t:"%F3" }, { n:0x9ABB, t:"%B4" }, { n:0xDABB, t:"%25" }, { n:0xEABB, t:"10" }, { n:0xFABB, t:"" }, { n:0x7BBB, t:"%E8" }, { n:0x8BBB, t:"%F2" }, { n:0x9BBB, t:"%60" }, { n:0xABBB, t:"%u03C6" }, { n:0xCBBB, t:"%u03C3" }, { n:0xDBBB, t:"%u2026" }, { n:0xEBBB, t:"%u2190" }, { n:0xFBBB, t:"" }, { n:0x7CBB, t:"%EA" }, { n:0x8CBB, t:"%F4" }, { n:0x9CBB, t:"%A8" }, { n:0xACBB, t:"%u03A9" }, { n:0xCCBB, t:"%u03C4" }, { n:0xDCBB, t:"%u2220" }, { n:0xECBB, t:"%u2192" }, { n:0xFCBB, t:"" }, { n:0x7DBB, t:"%EB" }, { n:0x8DBB, t:"%F6" }, { n:0x9DBB, t:"%BF" }, { n:0xCDBB, t:"%CD" }, { n:0xDDBB, t:"%DF" }, { n:0xEDBB, t:"%u2191" }, { n:0xFDBB, t:"" }, { n:0x6EBB, t:"%C1" }, { n:0x8EBB, t:"%DA" }, { n:0x9EBB, t:"%A1" }, { n:0xAEBB, t:"%u03C7" }, { n:0xEEBB, t:"%u2193" }, { n:0xFEBB, t:"" }, { n:0x6FBB, t:"%C0" }, { n:0x7FBB, t:"%CC" }, { n:0x8FBB, t:"%D9" }, { n:0x9FBB, t:"%u03B1" }, { n:0xCFBB, t:"%7E" }, { n:0xFFBB, t:"" }, ],,
// var str = ""; for(var i in arrs) { var x = String.fromCharCode(parseInt(i) + "A".charCodeAt(0)); str += "u16 ALL_" + x + "[] = {"; for(var j in arrs[i]) { str += "0x" + arrs[i][j].n.toString(16).toUpperCase() + ","; } str += "};\n"; } copy(str); console.log(str);
*/

// NOTE: The full catalog and each section takes up 2210 bytes total

const u16 ALL_A[] = {0x1662,0xB0BB,0x41,0x4FBB,0x40,0xB2,0x6ABB,0x72,0x3AEF,0x28BB,0x59BB,0x87E,0x68BB,0x97E,0x6BBB,0x14,0x6CBB,0x68EF,0x7AEF,};
const u16 ALL_B[] = {0x1762,0x42,0xB1BB,0x2BB,0x41EF,0x43EF,0x47EF,0x5,0x16BB,0x15BB,0x6CEF,0x5BEF,0x64EF,};
const u16 ALL_C[] = {0x1862,0x43,0xB2BB,0x3163,0xC4,0x25BB,0xCA,0xA5,0x93EF,0x29BB,0x37EF,0xC5,0x10EF,0xE1,0x85,0x47E,0xA1EF,0xFA,0x2E,0xFEF,0xFB,0xCB,0x2EF,0x57E,0xA2EF,0x67E,0x6DBB,0x52BB,0x57BB,0x7BEF,0x69EF,};
const u16 ALL_D[] = {0x1962,0x44,0xB3BB,0x2762,0x7BB,0x3BEF,0xB5,0xDB,0xB3,0x77E,0xDE,0xA9,0x2,0x65,0x1,0x54BB,0x6EF,0x75EF,0xA8,0x4FEF,0x7D,0xE5,0xDF,0x7C,0x66BB,0x6AEF,0x67BB,0x6BEF,};
const u16 ALL_E[] = {0x1A62,0x45,0x3B,0x31BB,0xB4BB,0xBF,0xD4,0x68,0x98EF,0xD0,0x2ABB,0x6BB,0xF5,0x50BB,0x12EF,0x3A62,0x51BB,0x3C62,0x3B62,0x55BB,0x9EEF,};
const u16 ALL_F[] = {0x2662,0x46,0xB5BB,0xAFBB,0x2F63,0x61,0x161,0x261,0x361,0x461,0x561,0x661,0x761,0x861,0x961,0x73,0xD3,0x14BB,0x3CEF,0x1EBB,0xE2,0x27,0x76,0x28,0x96,0x75,0x97,0x69,0xBA,0x24,0xA7E,0x3,0xB7E,0x3762,0x3962,0x3862,0x3DEF,};
const u16 ALL_G[] = {0x47,0xB6BB,0x64BB,0x9BB,0xE8,0x4EEF,0xD7,0x45EF,0xAD,0x53BB,0x9EF,0xAEF,0x5AEF,0xCEF,0xDEF,0x7EF,0x8EF,0x1ABB,0x19BB,0x65EF,0x45BB,0xCEBB,};
const u16 ALL_H[] = {0x48,0xB7BB,0x74,0xFC,0xA6,};
const u16 ALL_I[] = {0x2C63,0x49,0x2C,0xB8BB,0xCE,0xDA,0xB1,0x1BB,0x27BB,0x13EF,0x50EF,0x51EF,0x52EF,0x53EF,0x54EF,0x55EF,0x56EF,0x57EF,0x58EF,0x59EF,0xB9,0x4BB,0xDC,0x11BB,0x95EF,0xB4,0xFBB,0xEEF,0x7B,0x7A,0xA0EF,0xA4EF,};
const u16 ALL_J[] = {0x4A,0xB9BB,};
const u16 ALL_K[] = {0x4B,0xBABB,};
const u16 ALL_L[] = {0x5D,0x15D,0x25D,0x35D,0x45D,0x55D,0x4C,0xBCBB,0xBE,0xD6,0x8BB,0xC0,0x9C,0x92EF,0xF6,0x3262,0x2CBB,0x2BBB,0x49EF,0x4CEF,0xC7E,0x34EF,0xD7E,0x33BB,0xF4,0xFF,0x15EF,0x3ABB,0x34BB,};
const u16 ALL_M[] = {0x4D,0xBDBB,0x1362,0x1A,0x19,0x21,0xE6,0x862,0xA62,0x962,0xB62,0xF8,0x1F,0x44EF,0x4DEF,0x16EF,0x36EF,0x5ABB,0x39BB,0x1EEF,};
const u16 ALL_N[] = {0x2B63,0x262,0x2162,0x2D62,0x3062,0x4E,0xBEBB,0x38EF,0x95,0x94,0xB8,0xBB,0x1F63,0x1D63,0x48EF,0x5BB,0x25,0x66,0x3F,0x10BB,0x1BBB,0x5BBB,};
const u16 ALL_O[] = {0x4F,0xBFBB,0x3C,0x46EF,0xE0,0x11EF,};
const u16 ALL_P[] = {0x2262,0x2862,0x2962,0x2A62,0x50,0xC0BB,0x2D63,0x3063,0xADBB,0x60,0x160,0x260,0x360,0x460,0x560,0x660,0x760,0x860,0x960,0x2E63,0xB7,0x5F,0xEC,0xED,0xEE,0x9E,0x77,0x78,0xD8,0x9F,0x3BB,0xA1,0x4BBB,0x4CBB,0xA2,0x1D,0x1E,0xF7,0xDD,0x27E,0xE9,0x13,0x79EF,0xA0,0x30BB,0xEA,0x3463,0x44BB,0x43BB,0xA3,0xA6EF,0x1B63,0x3EBB,0x3FBB,0x18BB,0x17BB,0x91,0xA3EF,};
const u16 ALL_Q[] = {0x1462,0x1562,0x51,0xC1BB,0xF9,0x2F,0xA5EF,0x81EF,};
const u16 ALL_R[] = {0x1262,0x3562,0x3662,0x405E,0x415E,0x425E,0x435E,0x445E,0x455E,0x52,0xA,0xC2BB,0x42EF,0x2DBB,0x18,0x17,0x16,0x26BB,0x4DBB,0xAB,0x2EBB,0x20,0x162,0x94EF,0x12,0x64,0x37E,0x4EBB,0xD2,0x1B,0xD5,0xBBB,0x2FBB,0xABB,0x1C,0x15,0xD0BB,0x1FBB,0x9B,0x99,0x32EF,0x35EF,};
const u16 ALL_S[] = {0x3462,0x53,0xC3BB,0x2C62,0x2F62,0x67,0x23,0x79,0xC2,0xCBB,0xB6,0xAA,0x1AA,0x2AA,0x3AA,0x4AA,0x5AA,0x6AA,0x7AA,0x8AA,0x9AA,0x3162,0xE7,0xC8,0x90EF,0x91EF,0x8FEF,0xD9,0xA4,0x22,0xE3,0x17E,0xE4,0x38BB,0x36BB,0x58BB,0x32BB,0xDBB,0xC3,0xFE,0x1EF,0x37BB,0xC9,0x9A,0x98,0x3EF,0x49BB,0x4EF,0x42BB,0xBEF,0x35BB,0x47BB,0x46BB,0x3DBB,0x7E,0x4ABB,0x56BB,};
const u16 ALL_T[] = {0x2462,0x54,0xE,0xC4BB,0xDFBB,0x2163,0xC6,0x12BB,0xCC,0x1CBB,0xF7E,0xCF,0x74EF,0xE63,0xF63,0x21BB,0x93,0x23BB,0x84,0xEF,0x2263,0x24BB,0x3CBB,0x22BB,0x20BB,0xC7,0xA7,0x5EF,0x40BB,0xCD,0x1A63,0x2A63,0x97EF,0x3863,0x48BB,0x67EF,0x73EF,};
const u16 ALL_U[] = {0x805E,0x55,0xC5BB,0x663,0x82EF,0x85EF,0x88EF,0x8BEF,0x39EF,0x463,0x3362,0x107E,0x127E,0x9FEF,0x69BB,};
const u16 ALL_V[] = {0x815E,0x56,0xC6BB,0x763,0x83EF,0x86EF,0x89EF,0x8CEF,0x563,0x117E,0xEBB,0x9D,0xF3,0xF2,};
const u16 ALL_W[] = {0x825E,0x57,0xC7BB,0x84EF,0x87EF,0x8AEF,0x8DEF,0xE7E,0x96EF,0xD1,0x4BEF,0x3263,};
const u16 ALL_X[] = {0x2562,0x2663,0x362,0x462,0x562,0x762,0x1B62,0x1C62,0x1D62,0x2B62,0x2E62,0x58,0xF0BB,0xC8BB,0xDEBB,0x662,0x205E,0x225E,0x245E,0x265E,0x285E,0x2A5E,0x1162,0x3D,0x263,0xA63,0xB63,0x3663,0x2863,0x13BB,0x1DBB,0xFD,};
const u16 ALL_Y[] = {0x2763,0xC62,0xD62,0xE62,0x1062,0x1E62,0x1F62,0x2062,0x105E,0x115E,0x125E,0x135E,0x145E,0x155E,0x165E,0x175E,0x185E,0x195E,0x59,0xC9BB,0xF62,0x215E,0x235E,0x255E,0x275E,0x295E,0x2B5E,0x363,0xC63,0xD63,0x2963,0x4AEF,};
const u16 ALL_Z[] = {0x2362,0x5A,0xCABB,0x1663,0x1763,0x88,0x18EF,0x19EF,0x1AEF,0x1BEF,0x1CEF,0x1DEF,0x2063,0x1E63,0x87,0x1863,0x1963,0x63,0x163,0x2563,0x1263,0x1463,0x1363,0x1563,0x3BBB,0x3763,0x89,0x863,0x963,0x3363,0x2463,0x90,0x65BB,0x8B,0x92,0x8A,0x8E,0x8C,0x8F,0x86,0x17EF,0x41BB,0x3563,0x8D,0x1C63,};
const u16 ALL_SYMBOLS[] = {0x1063,0x1163,0x2363,0x5C,0x15C,0x25C,0x35C,0x45C,0x55C,0x65C,0x75C,0x85C,0x95C,0x62,0x10,0x30,0x70,0x11,0x31,0x71,0x32,0x33,0x4,0x34,0x35,0x6,0x36,0x7,0x37,0x8,0x38,0x9,0x29,0x39,0x2A,0x3A,0x6A,0xB,0x2B,0x5B,0x6B,0xC,0x6C,0xD,0x2D,0x6D,0x3E,0x6E,0xF,0x6F,0x7F,0x80,0x81,0xB0,0xF0,0xC1,0xF1,0x82,0x83,0xEB,0xAC,0xBC,0xBD,0xAE,0xAF,0x70BB,0x80BB,0x90BB,0xA0BB,0xE0BB,0x71BB,0x81BB,0x91BB,0xA1BB,0xD1BB,0xE1BB,0xF1BB,0x72BB,0x82BB,0x92BB,0xA2BB,0xD2BB,0xE2BB,0xF2BB,0x73BB,0x83BB,0x93BB,0xA3BB,0xD3BB,0xE3BB,0xF3BB,0x74BB,0x84BB,0x94BB,0xA4BB,0xD4BB,0xE4BB,0xF4BB,0x75BB,0x85BB,0x95BB,0xA5BB,0xD5BB,0xE5BB,0xF5BB,0x76BB,0x86BB,0x96BB,0xA6BB,0xD6BB,0xE6BB,0xF6BB,0x77BB,0x87BB,0x97BB,0xA7BB,0xD7BB,0xE7BB,0x78BB,0x88BB,0x98BB,0xA8BB,0xD8BB,0xE8BB,0x79BB,0x89BB,0x99BB,0xA9BB,0xD9BB,0xE9BB,0x7ABB,0x8ABB,0x9ABB,0xDABB,0xEABB,0x7BBB,0x8BBB,0x9BBB,0xABBB,0xCBBB,0xDBBB,0xEBBB,0x7CBB,0x8CBB,0x9CBB,0xACBB,0xCCBB,0xDCBB,0xECBB,0x7DBB,0x8DBB,0x9DBB,0xCDBB,0xDDBB,0xEDBB,0x6EBB,0x8EBB,0x9EBB,0xAEBB,0xEEBB,0x6FBB,0x7FBB,0x8FBB,0x9FBB,0xCFBB};

// TODO: Replace less used menus (like DISTR and DISTR_DRAW) with menus containing commonly used TI-BASIC tokens that are hard to access?

const u16 CTRL[] = { 0xCE, 0xCF, 0xD0, 0xD3, 0xD1, 0xD2, 0xD4, 0xD8, 0xD6,
               0xD7, 0x96EF, 0xDA, 0xDB, 0xE6, 0x5F, 0xD5, 0xD9, 0x54BB, 0x45BB,
               0x65EF, 0x11EF, 0x12EF };
const u16 IO[]   = { 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE0, 0xAD, 0xE1, 0xFB,
               0x53BB, 0xE8, 0xE7, 0x98EF, 0x2ABB, 0x97EF, 0x56BB };
const u16 COLOR[] = { 0x41EF, 0x42EF, 0x43EF, 0x44EF, 0x45EF, 0x46EF, 0x47EF, 0x48EF, 0x49EF, 0x4AEF,
                0x4BEF, 0x4CEF, 0x4DEF, 0x4EEF, 0x4FEF };

const u16 TEST[] = { 0x6A, 0x6F, 0x6C, 0x6E, 0x6B, 0x6D };
const u16 LOGIC[] = { 0x40, 0x3C, 0x3D, 0xB8 };

const u16 STRINGS[] = { 0x00AA, 0x01AA, 0x02AA, 0x03AA, 0x04AA, 0x05AA, 0x06AA, 0x07AA, 0x08AA, 0x09AA };
const u16 GDBS[] = { 0x0061, 0x0161, 0x0261, 0x0361, 0x0461, 0x0561, 0x0661, 0x0761, 0x0861, 0x0961 };
const u16 PICTURES[] = { 0x0060, 0x0160, 0x0260, 0x0360, 0x0460, 0x0560, 0x0660, 0x0760, 0x0860, 0x0960 };
const u16 EQUATIONS[] = { 0x105E, 0x115E, 0x125E, 0x135E, 0x145E, 0x155E, 0x165E, 0x175E, 0x185E, 0x195E,
                    0x205E, 0x215E, 0x225E, 0x235E, 0x245E, 0x255E, 0x265E, 0x275E, 0x285E, 0x295E, 0x2A5E, 0x2B5E,
                    0x405E, 0x415E, 0x425E, 0x435E, 0x445E, 0x455E,
                    0x805E, 0x815E, 0x825E };

const u16 MATRIX_NAMES[] = { 0x005C, 0x015C, 0x025C, 0x035C, 0x045C, 0x055C, 0x065C, 0x075C, 0x085C, 0x095C };
const u16 MATRIX_MATH[] = { 0xB3, 0x0E, 0xB5, 0xE2, 0xB4, 0x20, 0x14, 0x39BB, 0x3ABB, 0x29BB, 0x2DBB, 0x2EBB,
                      0x15, 0x16, 0x17, 0x18 };

const u16 MATH[] = { 0x03, 0x02, 0x0F, 0xBD, 0xF1, 0x27, 0x28, 0x25, 0x24, 0x33EF, 0x34EF, 0xA6EF };
const u16 NUM[] = { 0xB2, 0x12, 0xB9, 0xBA, 0xB1, 0x1A, 0x19, 0x08, 0x09, 0x32EF };
const u16 COMPLEX[] = { 0x25BB, 0x26BB, 0x27BB, 0x28BB, 0xB2, 0x2FBB, 0x30BB };
const u16 PROBABILITY[] = { 0xAB, 0x94, 0x95, 0x2D, 0x0ABB, 0x1FBB, 0x0BBB, 0x35EF };
const u16 FRACTION[] = { 0x30EF, 0x31EF, 0x39EF, 0x38EF };

const u16 DRAW[]   = { 0x85, 0x9C, 0xA6, 0x9D, 0xA7, 0xA9, 0xA4, 0xA8, 0xA5, 0x93, 0x67EF };
const u16 POINTS[] = { 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0x13 };
const u16 STORE[]  = { 0x98, 0x99, 0x9A, 0x9B };
const u16 DRAW_BACKGROUND[] = { 0x5BEF, 0x64EF };

const u16 DISTR[]      = { 0x1BBB, 0x10BB, 0x11BB, 0x13EF, 0x1CBB, 0x12BB, 0x1DBB, 0x13BB, 0x1EBB,
                     0x14BB, 0x15BB, 0x16BB, 0x95EF, 0x17BB, 0x18BB, 0x19BB, 0x1ABB };
const u16 DISTR_DRAW[] = { 0x35BB, 0x36BB, 0x37BB, 0x38BB };

const u16 ANGLE[] = { 0x0B, 0xAE, 0x0A, 0x01, 0x1B, 0x1C, 0x1D, 0x1E };

const u16 STAT_EDIT[] = { 0xE3, 0xE4, 0xFA, 0x4A };
const u16 STAT_CALC[] = { 0xF2, 0xF3, 0xF8, 0xFF, 0xF9, 0x2E, 0x2F, 0xF4,
                    0xF6, 0xF5, 0xF7, 0x33BB, 0x32BB, 0x16EF };
const u16 STAT_TESTS[] = { 0x3BBB, 0x3CBB, 0x3DBB, 0x3EBB, 0x3FBB, 0x41BB, 0x48BB, 0x42BB, 0x49BB, 0x43BB, 0x44BB,
                     0x40BB, 0x14EF, 0x47BB, 0x34BB, 0x15EF, 0x59BB };

const u16 LIST_OPS[]  = { 0xE3, 0xE4, 0xB5, 0xE2, 0x23, 0x29BB, 0x2CBB, 0x58BB, 0x14, 0x3ABB, 0x39BB, 0xEB };
const u16 LIST_MATH[] = { 0x1A, 0x19, 0x21, 0x1F, 0xB6, 0xB7, 0x0DBB, 0x0EBB };

TokenDirectory directories[] = {
    #define DIR_PRGM 0
    { .list_count = 4, .lists = {
        { .name = "Ctrl" , .name_count = 4, .tokens = CTRL , .tokens_count = ARRLEN(CTRL) },
        { .name = "I/O"  , .name_count = 3, .tokens = IO   , .tokens_count = ARRLEN(IO) },
        { .name = "Color", .name_count = 5, .tokens = COLOR, .tokens_count = ARRLEN(COLOR) },
        // NOTE: This gets hardcoded override treatment
        { .name = "Exec" , .name_count = 4, .tokens = null , .tokens_count = 0 },
    } },

    #define DIR_TEST 1
    { .list_count = 2, .lists = {
        { .name = "Test" , .name_count = 4, .tokens = TEST , .tokens_count = ARRLEN(TEST) },
        { .name = "LOGIC", .name_count = 5, .tokens = LOGIC, .tokens_count = ARRLEN(LOGIC) },
    } },

    #define DIR_VARS 2
    { .list_count = 4, .lists = {
        { .name = "Strings"  , .name_count = 7, .tokens = STRINGS  , .tokens_count = ARRLEN(STRINGS) },
        { .name = "Pictures" , .name_count = 8, .tokens = PICTURES , .tokens_count = ARRLEN(PICTURES) },
        { .name = "GDBs"     , .name_count = 4, .tokens = GDBS     , .tokens_count = ARRLEN(GDBS) },
        { .name = "Equations", .name_count = 9, .tokens = EQUATIONS, .tokens_count = ARRLEN(EQUATIONS) },
    } },

    #define DIR_MATRIX 3
    { .list_count = 2, .lists = {
        { .name = "Names", .name_count = 5, .tokens = MATRIX_NAMES, .tokens_count = ARRLEN(MATRIX_NAMES) },
        { .name = "Math" , .name_count = 4, .tokens = MATRIX_MATH , .tokens_count = ARRLEN(MATRIX_MATH) },
    } },

    #define DIR_ALL 4
    { .list_count = 27, .lists = {
        { .name = "A", .name_count = 1, .tokens = ALL_A, .tokens_count = ARRLEN(ALL_A) },
        { .name = "B", .name_count = 1, .tokens = ALL_B, .tokens_count = ARRLEN(ALL_B) },
        { .name = "C", .name_count = 1, .tokens = ALL_C, .tokens_count = ARRLEN(ALL_C) },
        { .name = "D", .name_count = 1, .tokens = ALL_D, .tokens_count = ARRLEN(ALL_D) },
        { .name = "E", .name_count = 1, .tokens = ALL_E, .tokens_count = ARRLEN(ALL_E) },
        { .name = "F", .name_count = 1, .tokens = ALL_F, .tokens_count = ARRLEN(ALL_F) },
        { .name = "G", .name_count = 1, .tokens = ALL_G, .tokens_count = ARRLEN(ALL_G) },
        { .name = "H", .name_count = 1, .tokens = ALL_H, .tokens_count = ARRLEN(ALL_H) },
        { .name = "I", .name_count = 1, .tokens = ALL_I, .tokens_count = ARRLEN(ALL_I) },
        { .name = "J", .name_count = 1, .tokens = ALL_J, .tokens_count = ARRLEN(ALL_J) },
        { .name = "K", .name_count = 1, .tokens = ALL_K, .tokens_count = ARRLEN(ALL_K) },
        { .name = "L", .name_count = 1, .tokens = ALL_L, .tokens_count = ARRLEN(ALL_L) },
        { .name = "M", .name_count = 1, .tokens = ALL_M, .tokens_count = ARRLEN(ALL_M) },
        { .name = "N", .name_count = 1, .tokens = ALL_N, .tokens_count = ARRLEN(ALL_N) },
        { .name = "O", .name_count = 1, .tokens = ALL_O, .tokens_count = ARRLEN(ALL_O) },
        { .name = "P", .name_count = 1, .tokens = ALL_P, .tokens_count = ARRLEN(ALL_P) },
        { .name = "Q", .name_count = 1, .tokens = ALL_Q, .tokens_count = ARRLEN(ALL_Q) },
        { .name = "R", .name_count = 1, .tokens = ALL_R, .tokens_count = ARRLEN(ALL_R) },
        { .name = "S", .name_count = 1, .tokens = ALL_S, .tokens_count = ARRLEN(ALL_S) },
        { .name = "T", .name_count = 1, .tokens = ALL_T, .tokens_count = ARRLEN(ALL_T) },
        { .name = "U", .name_count = 1, .tokens = ALL_U, .tokens_count = ARRLEN(ALL_U) },
        { .name = "V", .name_count = 1, .tokens = ALL_V, .tokens_count = ARRLEN(ALL_V) },
        { .name = "W", .name_count = 1, .tokens = ALL_W, .tokens_count = ARRLEN(ALL_W) },
        { .name = "X", .name_count = 1, .tokens = ALL_X, .tokens_count = ARRLEN(ALL_X) },
        { .name = "Y", .name_count = 1, .tokens = ALL_Y, .tokens_count = ARRLEN(ALL_Y) },
        { .name = "Z", .name_count = 1, .tokens = ALL_Z, .tokens_count = ARRLEN(ALL_Z) },
        { .name = "[", .name_count = 1, .tokens = ALL_SYMBOLS, .tokens_count = ARRLEN(ALL_SYMBOLS) },
    } },

    #define DIR_MATH 5
    { .list_count = 4, .lists = {
        { .name = "Math"       , .name_count = 4 , .tokens = MATH       , .tokens_count = ARRLEN(MATH) },
        { .name = "Number"     , .name_count = 6 , .tokens = NUM        , .tokens_count = ARRLEN(NUM) },
        { .name = "Complex"    , .name_count = 7 , .tokens = COMPLEX    , .tokens_count = ARRLEN(COMPLEX) },
        { .name = "Probability", .name_count = 11, .tokens = PROBABILITY, .tokens_count = ARRLEN(PROBABILITY) },
        { .name = "Fraction"   , .name_count = 8 , .tokens = FRACTION   , .tokens_count = ARRLEN(FRACTION) },
    } },

    #define DIR_DRAW 6
    { .list_count = 4, .lists = {
        { .name = "Draw"      , .name_count = 4 , .tokens = DRAW, .tokens_count = ARRLEN(DRAW) },
        { .name = "Points"    , .name_count = 6 , .tokens = POINTS, .tokens_count = ARRLEN(POINTS) },
        { .name = "Store"     , .name_count = 5 , .tokens = STORE, .tokens_count = ARRLEN(STORE) },
        { .name = "Background", .name_count = 10, .tokens = DRAW_BACKGROUND, .tokens_count = ARRLEN(DRAW_BACKGROUND) },
    } },

    #define DIR_ANGLE 7
    { .list_count = 1, .lists = {
        { .name = "Angle", .name_count = 5, .tokens = ANGLE, .tokens_count = ARRLEN(ANGLE) },
    } },

    #define DIR_DISTR 8
    { .list_count = 2, .lists = {
        { .name = "Distribute", .name_count = 10, .tokens = DISTR     , .tokens_count = ARRLEN(DISTR) },
        { .name = "Draw"      , .name_count = 4 , .tokens = DISTR_DRAW, .tokens_count = ARRLEN(DISTR_DRAW) },
    } },

    #define DIR_LIST 9
    { .list_count = 3, .lists = {
        // NOTE: Hardcoded over-ride for user-named lists
        { .name = "Names"    , .name_count = 5, .tokens = null    , .tokens_count = 0 },
        { .name = "Operators", .name_count = 9, .tokens = LIST_OPS, .tokens_count = ARRLEN(LIST_OPS) },
        { .name = "Math"     , .name_count = 4, .tokens = LIST_MATH, .tokens_count = ARRLEN(LIST_MATH) },
    } },

    #define DIR_STAT 10
    { .list_count = 3, .lists = {
        { .name = "Edit"     , .name_count = 4, .tokens = STAT_EDIT , .tokens_count = ARRLEN(STAT_EDIT) },
        { .name = "Calculate", .name_count = 9, .tokens = STAT_CALC , .tokens_count = ARRLEN(STAT_CALC) },
        { .name = "Tests"    , .name_count = 5, .tokens = STAT_TESTS, .tokens_count = ARRLEN(STAT_TESTS) },
    } },
};

#define Delta_InsertTokens 0
#define Delta_RemoveTokens 1
typedef u8 DeltaType;

// NOTE: When Delta.type == Delta_RemoveTokens,
// the structure is followed by Delta.remove_data.count bytes in memory
typedef struct Delta {
    DeltaType type;
    s24 cursor_was;
    union {
        struct { 
            s24 at;
            u16 count;
        } insert_data;
        struct {
            s24 at;
            u16 count;
        } remove_data;
    };
} Delta;

typedef struct DeltaCollection {
    // NOTE: This is an array where the first element is the oldest undo.
    // Newest undos are put towards the top.
    u8 data[4096];
    u24 data_size;
    u24 delta_count;
} DeltaCollection;

u24 size_of_delta(Delta *delta) {
    u24 result = 0;
    if(delta->type == Delta_InsertTokens) {
        result = sizeof(Delta);    
    } else if(delta->type == Delta_RemoveTokens) {
        result = sizeof(Delta) + cast(u24)delta->remove_data.count;
    } else { assert(false, "Delta with invalid type of %d\n", delta->type); }
    return result;
}

void clear_delta_collection(DeltaCollection *collection) {
    collection->data_size = 0;
    collection->delta_count = 0;
}

// NOTE: May return null if no room to add the delta
// data_size indicates how much data is copied from void *data.
// push_size indicates how much room is added to collection's array for the result.
Delta* push_delta(DeltaCollection *collection, void *data, u24 data_size, u24 push_size) {
    Delta *result = 0;
    if(push_size <= ARRLEN(collection->data)) {
        while(collection->data_size + push_size > ARRLEN(collection->data)) {
            u24 to_free = size_of_delta((Delta*)&collection->data[0]);
            u24 remaining = collection->data_size - to_free;
            copy(collection->data + to_free, collection->data, (s24)remaining);
            collection->data_size = remaining;
            collection->delta_count -= 1;
        }
        result = cast(Delta*)(collection->data + collection->data_size);
        copy(data, result, cast(s24)data_size);
        collection->data_size += push_size;
        collection->delta_count += 1;
    }
    return result;
}

// NOTE: Returns null if nothing to pop
Delta* pop_delta(DeltaCollection *collection) {
    Delta *result = 0;
    if(collection->delta_count > 0) {
        result = cast(Delta*)collection->data;
        u24 i;
        for(i = 0; i < collection->delta_count; ++i) {
            if(i == collection->delta_count - 1) {
                break;
            }
            result = cast(Delta*)((cast(u8*)result) + size_of_delta(result));
        }
        assert(i == collection->delta_count - 1, "Should just be true");
        collection->delta_count -= 1;
        collection->data_size -= size_of_delta(result);
    }
    return result;
}

void push_insert_delta(DeltaCollection *collection, s24 cursor_was, s24 inserted_at, u16 inserted_count) {
    u24 to_push = sizeof(Delta);
    Delta delta;
    delta.type = Delta_InsertTokens;
    delta.cursor_was = cursor_was;
    delta.insert_data.at = inserted_at;
    delta.insert_data.count = inserted_count;
    Delta *result = push_delta(collection, &delta, sizeof(Delta), to_push);
    I_KNOW_ITS_UNUSED(result);
}

void push_remove_delta(DeltaCollection *collection, s24 cursor_was, s24 removed_at, void *removed_data, u16 removed_count) {
    u24 to_push = sizeof(Delta) + removed_count;
    Delta delta;
    delta.type = Delta_RemoveTokens;
    delta.cursor_was = cursor_was;
    delta.remove_data.at = removed_at;
    delta.remove_data.count = removed_count;
    Delta *placed_at = push_delta(collection, &delta, sizeof(Delta), to_push);
    if(placed_at) {
        u8 *data_goes_at = (cast(u8*)placed_at) + sizeof(Delta);
        copy(removed_data, data_goes_at, removed_count);
    }
}

typedef enum CursorMode {
    CursorMode_Normal,
    CursorMode_Second,
    CursorMode_Alpha,
} CursorMode;

typedef struct Linebreak {
    // NOTE: Use get_linebreak_location to read linebreak location, because
    // we want the 0th one to always be -1, but we want to be able to store
    // a u16 from 0-65535, and we want to save storage space
    u16 location_;
    u8 indentation;
} Linebreak;

typedef struct LoadedProgram {
    bool program_loaded;
    u8 program_name[9]; // NOTE: Null terminated. Max 8 chars.
    s24 view_top_program;
    s24 selected_program;
    bool archived;

    // #define PROGRAM_DATA_SIZE OS_VAR_MAX_SIZE
    #define PROGRAM_DATA_SIZE 44000
    u8 data[PROGRAM_DATA_SIZE];
    s24 size;
    
    // 4800 bytes
    Linebreak linebreaks[1600];
    s24 linebreaks_count;

    u16 linebreaks_dirty_indentation_min;

    s24 cursor;
    bool cursor_selecting;
    s24 cursor_started_selecting;

    s24 view_top_line;
    s24 view_first_character;

    // Fancy effect with lerping
    s24 scroller_visual_y;
    s24 undo_bar_visual_height;
    s24 redo_bar_visual_height;

    DeltaCollection undo_buffer;
    DeltaCollection redo_buffer;

    bool entering_goto;
    u8 entering_goto_chars[2];
    u8 entering_goto_chars_count;

    // NOTE: If this is a lot, then we reduce the max width to keep the program running smoothly
    u16 rendered_token_count_last_frame;

    // NOTE: Null if closed. If `opened_directory.name == "EXEC"`
    // then we have some hardcoded overrides to match TI-OS's functionality
    TokenDirectory *opened_directory;
    s24 opened_directory_list_index;
    s24 opened_directory_token_index;
    s24 opened_directory_view_top_token_index;
} LoadedProgram;

#define CLIPBOARD_APPVAR_NAME "AETHRCLP"
#define SETTINGS_DATA_APPVAR_NAME "AETHRDAT"
typedef struct EditorSettings {
    // NOTE: When I change the settings struct,
    // increment the version define by 1.
    // This will make it so old versions are invalidated on load.
    // This first member "settings_struct_version" must persist.
    #define CURRENT_SETTINGS_STRUCT_VERSION 0
    u8 settings_struct_version;

    bool light_mode;
    u8 last_editing_program[9];
    u16 last_cursor_y;
} EditorSettings;

typedef struct Editor {
    CursorMode cursor_mode;
    bool alpha_is_lowercase;

    bool running;
    bool run_program_at_end;
    // NOTE: For showing errors to user before we close
    char *exit_message_at_end; 
    // NOTE: Saved/loaded on program start/exit. Persistent.
    EditorSettings settings;

    u8 background_color;
    u8 foreground_color;
    u8 highlight_color;
} Editor;

static LoadedProgram program = {};
static Editor editor = {};

// 2500 bytes
typedef struct OS_Program {
    // NOTE: Not null terminated
    u8 name[8];
} OS_Program;
OS_Program os_programs[312];
s24        os_programs_count;

typedef struct OS_List {
    // NOTE: Not null terminated
    u8 name[5];
} OS_List;
OS_List os_lists[64];
s24     os_lists_count;

u24 alphabetical_sort_cost(char *name) {
    // NOTE: We sort by the first 4 letters.
    u24 result = 0;
    u24 magnitude = 26*26*26*26;

    for(int l = 0; l <= (4 - 1) && name[l] != 0; ++l) {
        u8 letter_index;
        if(name[l] >= 'A' && name[l] <= 'Z') letter_index = (u8)(name[l] - 'A');
        else if(name[l] >= 'a' && name[l] <= 'z') letter_index = (u8)(name[l] - 'a');
        else letter_index = 0;
        result += letter_index*magnitude;
        magnitude = magnitude / 26;
    }
    return result;
}

typedef struct RunPrgmCallbackReconstructProgram {
    u8 program_name[9];
    s24 cursor;
    s24 view_top_line;
} RunPrgmCallbackReconstructProgram;

u8 key_down[8];
u8 key_held[8];
u8 key_up[8];
u8 key_debounced[8];
u8 key_timers[8][8];
bool on_pressed;
bool on_held;

void gc_before() { gfx_End(); }
void gc_after() { gfx_Begin(); }
void exit_with_message(char *message) {
    editor.running = false;
    editor.exit_message_at_end = message;
}
void update_editor_theme_based_on_settings() {
    if(editor.settings.light_mode == false) {
        editor.background_color = 0x08;
        editor.foreground_color = 0xF7;
        editor.highlight_color = 0x7A;
    } else {
        editor.background_color = 0xFF;
        editor.foreground_color = 0x00;
        editor.highlight_color = 0xF7;
    }
}

int main() {

    {
        u8 editor_settings_handle = ti_Open(SETTINGS_DATA_APPVAR_NAME, "r");
        if(editor_settings_handle != 0) {
            u8 stored_settings_struct_version;
            u24 count_read = ti_Read(&stored_settings_struct_version, 1, 1, editor_settings_handle);
            if(count_read == 1 && stored_settings_struct_version == CURRENT_SETTINGS_STRUCT_VERSION) {
                ti_Seek(0,SEEK_SET,editor_settings_handle);
                ti_Read(&editor.settings, sizeof(EditorSettings), 1, editor_settings_handle);
            }
            ti_Close(editor_settings_handle);
        }
        editor.settings.settings_struct_version = CURRENT_SETTINGS_STRUCT_VERSION;
    }
    update_editor_theme_based_on_settings();

    editor.running = true;
    kb_DisableOnLatch();
    gfx_Begin();
    gfx_SetDrawBuffer();
    ti_SetGCBehavior(gc_before, gc_after);
    fontlib_SetFont(editor_font, 0);

    {    
        // NOTE: Scan OS for programs
        void *it = null;
        while(true) {
            char *name = ti_DetectVar(&it, null, OS_TYPE_PRGM);
            if(!name) {
                break;
            } else {
                bool name_valid = (name[0] != '!' && name[0] != '#');
                if(name_valid) {
                    os_programs_count += 1;
                    char *dest = (char*)os_programs[os_programs_count - 1].name;
                    log("Loading program %s\n", name);
                    for(char *val = name; val <= name + 7; val += 1, dest += 1) {
                        *dest = *val;
                        if(*val == 0) {
                            break;
                        }
                    }
                    if(os_programs_count == ARRLEN(os_programs)) {
                        break;
                    }
                }
            }
        }

        // NOTE: Bubble sort (could switch to radix later maybe, if startup time is long)
        for(int i = 0; i < os_programs_count - 1; ++i) {
            bool early_out = true;
            for(int j = 0; j < os_programs_count - (1+i); ++j) {
                s24 a = j;
                s24 b = j + 1;
                u24 a_cost = alphabetical_sort_cost((char*)os_programs[a].name);
                u24 b_cost = alphabetical_sort_cost((char*)os_programs[b].name);
                if(a_cost > b_cost) {
                    OS_Program a_temp = os_programs[a];
                    os_programs[a] = os_programs[b];
                    os_programs[b] = a_temp;
                    early_out = false;
                }
            }
            if(early_out) { break; }
        }
        // NOTE: Hard-coded functionality
        directories[DIR_PRGM].lists[3].tokens_count = cast(s16)os_programs_count;
    }

    {
        // NOTE: Scan OS for lists
        void *it = null;
        while(true) {
            char *name = ti_DetectVar(&it, null, OS_TYPE_REAL_LIST);
            if(!name) {
                break;
            } else {
                // NOTE: Lists start with 0x5D no matter what, and we don't care.
                name += 1;
                bool name_valid = true;
                for(u8 i = 0; i <= 5 - 1; ++i) {
                    if(i == 1 && name[i] >= 0 && name[i] <= 5) { name_valid = false; break; }
                }
                if(name_valid) {
                    os_lists_count += 1;
                    char *dest = (char*)os_lists[os_lists_count - 1].name;
                    log("Loading list %s\n", name);
                    for(char *val = name; val <= name + 5; val += 1, dest += 1) {
                        *dest = *val;
                        if(*val == 0) {
                            break;
                        }
                    }
                    if(os_lists_count == ARRLEN(os_lists)) {
                        break;
                    }
                }
            }
        }

        // NOTE: Bubble sort (could switch to radix later maybe, if startup time is long)
        for(int i = 0; i < os_lists_count - 1; ++i) {
            bool early_out = true;
            for(int j = 0; j < os_lists_count - (1+i); ++j) {
                s24 a = j;
                s24 b = j + 1;
                u24 a_cost = alphabetical_sort_cost((char*)os_lists[a].name);
                u24 b_cost = alphabetical_sort_cost((char*)os_lists[b].name);
                if(a_cost > b_cost) {
                    OS_List a_temp = os_lists[a];
                    os_lists[a] = os_lists[b];
                    os_lists[b] = a_temp;
                    early_out = false;
                }
            }
            if(early_out) { break; }
        }
        // NOTE: Hard-coded functionality
        directories[DIR_LIST].lists[0].tokens_count = cast(s16)os_lists_count;
    }
    
    #define TARGET_FRAMERATE (15)
    #define TARGET_CLOCKS_PER_FRAME cast(s24)((cast(u24)CLOCKS_PER_SEC) / TARGET_FRAMERATE)
    
    s24 clock_counter = 0;
    u24 previous_clock = cast(u24)clock();

#if DEBUG
    u24 last_frame = cast(u24)clock();
#endif

    // NOTE: Autosave causes a 50ms spike for an archived 5000 byte program.
    // I can't notice any spike at all for a non-archived 5000 byte program.
    #define AUTOSAVE_INTERVAL_MILLISECONDS (100)
    #define AUTOSAVE_INTERVAL_CLOCK_CYCLES ((AUTOSAVE_INTERVAL_MILLISECONDS*CLOCKS_PER_SEC)/1000)
    s24 clock_cycles_until_autosave = AUTOSAVE_INTERVAL_CLOCK_CYCLES;

    editor.run_program_at_end = false;
    editor.running = true;
    if(os_programs_count == 0) {
        exit_with_message("No TI-Basic programs found.");
    }
    while(editor.running) {
        u24 current_clock = cast(u24)clock();
        s24 diff = cast(s24)(current_clock - previous_clock);
        previous_clock = current_clock;
        clock_counter -= diff;
        if(program.program_loaded) {
            clock_cycles_until_autosave -= diff;
            if(clock_cycles_until_autosave <= 0) {
                clock_cycles_until_autosave = AUTOSAVE_INTERVAL_CLOCK_CYCLES;
                save_program(false);
            }
        }
        if(clock_counter <= 0) {
            clock_counter = TARGET_CLOCKS_PER_FRAME;
#if DEBUG
            u24 frame_time_ms = ((current_clock - last_frame) * 1000) / cast(u24)CLOCKS_PER_SEC;
            last_frame = current_clock;
            I_KNOW_ITS_UNUSED(frame_time_ms);
            // log("%dms (target %dms)\n",frame_time_ms, (TARGET_CLOCKS_PER_FRAME*1000)/cast(u24)CLOCKS_PER_SEC);
#endif

            update_input();
            update();
            render();
            gfx_SwapDraw();
        } else {
            // NOTE: If we're within 50 milliseconds of a frame, don't use sleep as the thread may not wake in time?
            // TODO: (but I don't know how inaccurate the timer is, and how close we can cut it)
            u16 ms_until_frame = cast(u16)((cast(u24)clock_counter * 1000) / cast(u24)CLOCKS_PER_SEC);
            if(ms_until_frame >= 50) msleep(ms_until_frame - 50);
        }
        
    }

    if(program.program_loaded) {
        save_program(true);
    }

    if(editor.exit_message_at_end != null) {
        #define DISPLAY_EXIT_MESSAGE_FOR_MILLISECONDS 5000
        #define DISPLAY_EXIT_MESSAGE_FOR_CLOCK_CYCLES ((DISPLAY_EXIT_MESSAGE_FOR_MILLISECONDS*CLOCKS_PER_SEC) / 1000)
        u24 message_length = 0;
        while(editor.exit_message_at_end[message_length] != 0) {
            message_length += 1;
        }
        u24 start = cast(u24)clock();
        gfx_FillScreen(0xFF);
        fontlib_SetForegroundColor(0x00);
        fontlib_SetTransparency(true);
        draw_string("Aether exited with message", 320/2 - (FONT_WIDTH*26)/2, 240/2 - FONT_HEIGHT - FONT_HEIGHT - 2);
        draw_string(editor.exit_message_at_end, 320/2 - (FONT_WIDTH*message_length)/2, 240/2 - FONT_HEIGHT);
        gfx_SwapDraw();
        while(clock() - start <= DISPLAY_EXIT_MESSAGE_FOR_CLOCK_CYCLES) {
            msleep(10);
        }
    }

    ti_SetGCBehavior(null, null);
    
    gfx_End();

    {
        u8 editor_settings_handle = ti_Open(SETTINGS_DATA_APPVAR_NAME, "w");
        if(editor_settings_handle != 0) {
            if(program.program_loaded) {
                assert(ARRLEN(editor.settings.last_editing_program) == ARRLEN(program.program_name), "Sizes should be equal");
                copy(cast(u8*)program.program_name, cast(u8*)editor.settings.last_editing_program, ARRLEN(editor.settings.last_editing_program));
                editor.settings.last_cursor_y = cast(u16)calculate_cursor_y();
            }
            ti_Write(&editor.settings, sizeof(EditorSettings), 1, editor_settings_handle);
            if(ti_ArchiveHasRoomVar(editor_settings_handle)) {
                ti_SetArchiveStatus(true, editor_settings_handle);
            }
            ti_Close(editor_settings_handle);
        }
    }

    ti_Delete(CLIPBOARD_APPVAR_NAME);

    if(editor.run_program_at_end && program.program_loaded) {
        RunPrgmCallbackReconstructProgram data;
        copy(program.program_name, data.program_name, 9);
        data.cursor = program.cursor;
        data.view_top_line = program.view_top_line;
        os_RunPrgm(cast(char*)program.program_name, cast(void*)&data, sizeof(data), run_prgm_callback);
    }
    
    return 0;
}

int run_prgm_callback(void *data_, int retval) {
    I_KNOW_ITS_UNUSED(retval);
    // TODO: By this point, I believe the first_token/current_token have been overwritten.
    // Look into what happens when we return from a program.
    // For running programs is disabled anyway
    // u24 *program_first_token_ptr   = cast(u24*)0x0D02317;
    // u24 *program_current_token_ptr = cast(u24*)0x0D0231A;
    // u24 offset = (*program_current_token_ptr) - (*program_first_token_ptr);
    // log("%u - %u = %u", (*program_current_token_ptr), (*program_first_token_ptr), offset);

    log("Program exited with code %d\n", retval);

    RunPrgmCallbackReconstructProgram *data = cast(RunPrgmCallbackReconstructProgram*)data_;
    load_program(cast(char*)data->program_name);
    program.cursor = data->cursor;
    program.view_top_line = data->view_top_line;
    return main();
}

// Takes a linebreak Y, returns offset into program.data
s24 get_linebreak_location(int i) {
    if(i == 0) {
        return -1;
    }
    return cast(s24)program.linebreaks[i].location_;
}

void draw_string(char* str, u24 x, u8 y) {
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawString(str);
}
void draw_string_max_chars(char* str, u24 max, u24 x, u8 y) {
    fontlib_SetCursorPosition(x, y);
    fontlib_DrawStringL(str, max);
}

void update_input(void) {
    kb_Scan();
    static uint8_t last_pressed[8];
    static uint8_t pressed_or_released[8];
    for(u8 i = 1; i < 8; i++) {
        pressed_or_released[i] = last_pressed[i] ^ kb_Data[i];
        key_up[i]   = ( last_pressed[i]) & pressed_or_released[i];
        key_down[i] = (~last_pressed[i]) & pressed_or_released[i];
        key_held[i] = kb_Data[i];
        last_pressed[i] = kb_Data[i];
        
        for(u8 bit_index = 0; bit_index < 8; ++bit_index) {
            u8 mask = (u8)((u8)1 << bit_index);
            
            if(key_held[i] & mask) {
                if(key_timers[i][bit_index] < 255) {
                    key_timers[i][bit_index] += 1;
                }
            } else {
                key_timers[i][bit_index] = 0;
            }
            
            if(key_timers[i][bit_index] == 1 ||
               key_timers[i][bit_index] >= 4) {
                key_debounced[i] |= mask;
            } else {
                key_debounced[i] &= (~mask);
            }
        }
    }

    on_pressed = false;
    if(kb_On) {
        if(!on_held) { on_pressed = true; }
        on_held = true;
    } else {
        on_held = false;
    }
}

void offset_linebreaks(s24 from_here, s24 offset_by) {
    for(int i = program.linebreaks_count - 1; i >= 0; --i) {
        if(get_linebreak_location(i) >= from_here) { program.linebreaks[i].location_ += offset_by; }
        else { break; }
    }
}

void make_room_for_tokens(s24 first_index, s24 count) {
    // NOTE: iterate backwards so we copy things forwards
    // Initially this function was only this following for loop,
    // but after unrolling the loop,
    // the editor is noticably faster at inserting tokens with large programs.
    // for(int i = program.size - 1; i >= first_index + count; --i) {
    //     program.data[i] = program.data[i - count];
    // }

    s24 max = program.size - 1;
    s24 min = first_index + count;
    s24 iterate_count = (max + 1) - min;
    s24 iterate_count_divided_by_4 = iterate_count / 4;
    s24 iterate_count_remainder = iterate_count - (iterate_count_divided_by_4 * 4);
    s24 it = max;
    while(iterate_count_divided_by_4 >= 1) {
        program.data[it - 0] = program.data[it - (count + 0)];
        program.data[it - 1] = program.data[it - (count + 1)];
        program.data[it - 2] = program.data[it - (count + 2)];
        program.data[it - 3] = program.data[it - (count + 3)];
        it -= 4;
        iterate_count_divided_by_4 -= 1;
    }
    while(iterate_count_remainder >= 1) {
        program.data[it] = program.data[it - count];
        it -= 1;
        iterate_count_remainder -= 1;
    }
}

// NOTE: Returns index of first linebreak that can be added
s24 make_room_for_linebreaks(s24 first_linebreaks_program_data_index, s24 count) {
    s24 result = -2;
    
    assert(count >= 1, "No room needed for linebreaks");
    for(s24 i = program.linebreaks_count - 1; i >= 0; --i) {
        if(get_linebreak_location(i) < first_linebreaks_program_data_index)
        {
            result = i+1;
            break;
        }
    }
    assert(result != -2, "failed");
    // NOTE: So result should be the index that we can set fresh values to.
    // Push future elements later in array
    program.linebreaks_count += count;
    for(s24 i = program.linebreaks_count - 1; i >= result + count; --i) {
        program.linebreaks[i] = program.linebreaks[i - count];
    }

    assert(result <= (program.linebreaks_count - 1) && result >= 1, "out-of-bounds");
    return result;
}

void copy(void *src, void *dest, s24 count) {
    for(s24 i = 0; i < count; ++i) {
        (cast(u8*)dest)[i] = (cast(u8*)src)[i];
    }
}

void zero(void *dest, s24 count) {
    for(s24 i = 0; i < count; ++i) {
        ((u8*)dest)[i] = 0;
    }
}

s24 calculate_line_y(s24 token_offset) {
    s24 result = program.linebreaks_count - 1;
    for(int i = 0; i <= program.linebreaks_count - 1; ++i) {
        if(token_offset <= get_linebreak_location(i)) {
            result = i - 1;
            break;
        }
    }
    return result;
}

s24 calculate_cursor_y(void) {
    return calculate_line_y(program.cursor);
}

// NOTE: When a line changes, only future lines get affected.
// So line+1 is what is marked as dirty
void mark_indentation_dirty_from_line_changed(s24 line_that_changed) {
    assert(program.linebreaks_dirty_indentation_min != 0, "Should never be 0");
    assert(line_that_changed >= 0 && line_that_changed <= 65536, "valid u16 range");
    program.linebreaks_dirty_indentation_min = min(program.linebreaks_dirty_indentation_min, cast(u16)line_that_changed + 1);
}

// NOTE: push_delta may be null if you do not want to push to an undo/redo buffer
void remove_tokens_(s24 at, u16 bytes_count, DeltaCollection* push_delta) {
    if(push_delta) {
        push_remove_delta(push_delta, program.cursor, at, program.data + at, bytes_count);
    }

    if(at < 0) {
        bytes_count += at;
        at = 0;
    }

    if(at <= program.size - 1) {
        s24 max = at + bytes_count - 1;
        if(max >= program.size) {
            max = program.size - 1;
            s24 new_count = ((max - at) + 1);
            assert(new_count >= 0 && new_count <= 65536, "Must be within u16 range");
            bytes_count = cast(u16)new_count;
        }
        if(bytes_count != 0) {
            s24 linebreaks_count = 0;
            for(s24 i = at; i <= at + (bytes_count - 1); ++i) {
                if(program.data[i] == LINEBREAK) {
                    linebreaks_count += 1;
                }
            }
            s24 first_linebreak = calculate_line_y(at) + 1;

            if(linebreaks_count >= 1) {
                for(s24 i = first_linebreak; i <= program.linebreaks_count - linebreaks_count - 1; ++i) {
                    program.linebreaks[i] = program.linebreaks[i + linebreaks_count];
                }
                program.linebreaks_count -= linebreaks_count;
            }

            {
                // NOTE: Unrolled version of the loop
                // for(s24 i = at; i <= program.size - (bytes_count - 1); ++i) {
                //     program.data[i] = program.data[i + bytes_count];
                // }
                s24 min = at;
                s24 max = program.size - (bytes_count - 1);
                s24 iterate_count = (max+1)-min;
                s24 iterate_count_by_4 = iterate_count/4;
                s24 iterate_count_remainder = iterate_count - (4*iterate_count_by_4);
                s24 it = min;
                while(iterate_count_by_4 >= 1) {
                    program.data[it+0] = program.data[it + 0 + bytes_count];
                    program.data[it+1] = program.data[it + 1 + bytes_count];
                    program.data[it+2] = program.data[it + 2 + bytes_count];
                    program.data[it+3] = program.data[it + 3 + bytes_count];
                    it += 4;
                    iterate_count_by_4 -= 1;
                }
                while(iterate_count_remainder >= 1) {
                    program.data[it] = program.data[it + bytes_count];
                    it += 1;
                    iterate_count_remainder -= 1;
                }
            }
            offset_linebreaks(at, -1 * cast(s24)bytes_count);
            program.size -= bytes_count;

            mark_indentation_dirty_from_line_changed(first_linebreak - 1);
        }

    }
}

void remove_tokens(s24 at, u16 bytes_count) {
    remove_tokens_(at, bytes_count, &program.undo_buffer);
    clear_delta_collection(&program.redo_buffer);
}

// NOTE: push_delta may be null if you do not want to push to an undo/redo buffer
void insert_tokens_(s24 at, u8 *tokens, u16 bytes_count, DeltaCollection* push_delta) {
    if(program.size + cast(s24)bytes_count < PROGRAM_DATA_SIZE) {
        program.size += bytes_count;
        offset_linebreaks(at, bytes_count);
        make_room_for_tokens(at, bytes_count);
        
        s24 linebreaks_to_add = 0;
        for(s24 i = 0; i < bytes_count; ++i) {
            if(tokens[i] == LINEBREAK) { linebreaks_to_add += 1; }
        }
        s24 add_linebreak_at;
        if(linebreaks_to_add >= 1) {
            add_linebreak_at = make_room_for_linebreaks(at, linebreaks_to_add);
        }
        s24 n = 0;
        for(s24 i = at; i <= at + bytes_count - 1; ++i, ++n) {
            program.data[i] = tokens[n];
            if(tokens[n] == LINEBREAK) {
                program.linebreaks[add_linebreak_at].location_ = cast(u16)i;
                add_linebreak_at += 1;
            }
        }
        
        if(push_delta) {
            push_insert_delta(push_delta, program.cursor, at, bytes_count);
        }

        s24 first_linebreak = calculate_line_y(at);
        mark_indentation_dirty_from_line_changed(first_linebreak);
    } else {
        assert(false, "Program too large");
    }
}

void insert_tokens(s24 at, u8 *tokens, u16 tokens_count) {
    insert_tokens_(at, tokens, tokens_count, &program.undo_buffer);
    clear_delta_collection(&program.redo_buffer);
}

void insert_token_u8(s24 at, u8 token) {
    insert_tokens(at, &token, 1);
}
void insert_token_u16(s24 at, u16 token) {
    insert_tokens(at, cast(u8*)&token, 2);
}

// It's intended to use this with an undo_delta, then pass the redo_buffer into put_undo_for_this_action_into_collection
// and the same for using redo_deltas and passing undo_buffers, so it's all reversible.
void apply_delta_to_program(Delta *delta, DeltaCollection *put_undo_for_this_action_into_collection) {
    assert(put_undo_for_this_action_into_collection != null, "Must be non-null");
    if(delta->type == Delta_InsertTokens) {
        remove_tokens_(delta->insert_data.at, delta->insert_data.count, put_undo_for_this_action_into_collection);
    }
    if(delta->type == Delta_RemoveTokens) {
        // NOTE: Data is stored after *delta structure pointer
        u8 *data = (cast(u8*)delta) + sizeof(Delta);
        insert_tokens_(delta->remove_data.at, data, delta->remove_data.count, put_undo_for_this_action_into_collection);
    }
    program.cursor = delta->cursor_was;
}

inline bool get_is_prgm_exec_override() {
    return program.opened_directory == &directories[DIR_PRGM] && program.opened_directory_list_index == 3;
}

inline bool get_is_list_name_override() {
    return program.opened_directory == &directories[DIR_LIST] && program.opened_directory_list_index == 0;
}

// NOTE: This ZEROES the global "static LoadedProgram program = {}" state!
void load_program(char *name) {
    blit_loading_indicator();
    zero(&program, sizeof(LoadedProgram));
    program.linebreaks[0].location_ = 0;
    program.linebreaks_count = 1;

    int n;
    for(n = 0; n <= 8 - 1; ++n) {
        if(name[n] == 0) { break; }
        program.program_name[n] = cast(u8)name[n];
    }
    program.program_name[n] = 0;

    bool fully_loaded_program = false;
    u8 load = ti_OpenVar((char*)program.program_name, "r", OS_TYPE_PRGM);
    assert(load != 0, "Cannot open file");
    if(load != 0) {
        if(ti_IsArchived(load)) {
            program.archived = true;
        } else {
            program.archived = false;
        }
        u24 size = cast(u24)ti_GetSize(load);
        assert(size <= PROGRAM_DATA_SIZE, "Program too big");
        assert(size <= 65536, "program.size is u16");
        if(size <= PROGRAM_DATA_SIZE) {
            u24 amount_read = ti_Read(program.data, 1, size, load);
            bool success = (size == amount_read);
            assert(success, "Failed to read. %d != %d", size, amount_read);
            if(success) {
                program.size = cast(u16)size;
                s24 indentation = 0;
                for(u16 i = 0; i <= program.size - 1;) {
                    u8 byte = program.data[i];
                    change_indentation_based_on_byte(indentation, byte);
                    if(byte == LINEBREAK) {
                        if(program.linebreaks_count == ARRLEN(program.linebreaks) - 1) {
                            assert(cast(u24)program.linebreaks_count < ARRLEN(program.linebreaks) - 1, "Too big");
                            exit_with_message("Program has too many line breaks!");
                            break;
                        } else {
                            program.linebreaks_count += 1;
                            program.linebreaks[program.linebreaks_count - 1].location_ = i;
                            program.linebreaks[program.linebreaks_count - 1].indentation = cast(u8)indentation;
                        }
                    }
                    i += get_token_size(i);
                }
                fully_loaded_program = true;
            } else {
                exit_with_message("Failed to read program.");
            }
        } else {
            exit_with_message("Program too big. (Max 42500 bytes)");
        }
        ti_Close(load);
    } else {
        exit_with_message("Program could not be read.");
        editor.running = false;
    }

    if(fully_loaded_program) {
        program.program_loaded = true;
        program.linebreaks_dirty_indentation_min = 1;
    }
}

void save_program(bool are_we_exiting_so_we_should_do_a_final_archiving_of_the_variable) {
    assert(program.program_loaded, "Program should be loaded");
    if(program.program_loaded) {
        u8 handle = ti_OpenVar((char*)program.program_name, "r", OS_TYPE_PRGM);
        u16 space_that_will_be_freed = 0;
        if(handle) {
            if(!ti_IsArchived(handle)) {
                space_that_will_be_freed = ti_GetSize(handle);
            }
            ti_Close(handle);
        }
        void *unused;
        u24 free_ram = os_MemChk(&unused);
        bool has_room = cast(s24)free_ram >= (cast(s24)program.size - cast(s24)space_that_will_be_freed);
        if(has_room) {
            // ti_DeleteVar(cast(char*)program.program_name, OS_TYPE_PRGM);
            handle = ti_OpenVar(cast(char*)program.program_name, "w", OS_TYPE_PRGM);
            u24 written = ti_Write(program.data, 1, cast(u24)program.size, handle);
            assert(cast(s24)written == program.size, "Didn't write full program... %d/%d", written, program.size);
            if(are_we_exiting_so_we_should_do_a_final_archiving_of_the_variable) {
                ti_SetArchiveStatus(program.archived, handle);
            }
            I_KNOW_ITS_UNUSED(written);
            ti_Close(handle);
        } else {
            // TODO: We probably want to open a "go archive some programs" wizard
            // so the user can still manage to save instead of losing their data...
            // The good thing is autosave will make this not so bad
            assert(false, "Not enough room to write appvar");
            exit_with_message("Not enough RAM to save");
        }
    }
}

void open_directory(u8 index) {
    program.opened_directory = &directories[index];
    program.opened_directory_list_index = 0;
    program.opened_directory_token_index = 0;
}

bool save_clipboard(s24 at, s24 size) {
    assert(size >= 0 && size <= PROGRAM_DATA_SIZE, "Too big clipboard save");
    bool success = false;
    u8 clipboard_handle = ti_Open(CLIPBOARD_APPVAR_NAME, "w");
    if(clipboard_handle) {
        u24 written = ti_Write(program.data + at, cast(u24)size, 1, clipboard_handle);
        if(written == 1) {
            success = true;
            if(ti_ArchiveHasRoomVar(clipboard_handle)) {
                ti_SetArchiveStatus(true, clipboard_handle);
            }
        }
        ti_Close(clipboard_handle);
        if(!success) { ti_Delete(CLIPBOARD_APPVAR_NAME); }
    }
    return success;
}

// NOTE: Returns number of bytes pasted
s24 paste_clipboard(s24 at) {
    s24 amount_pasted = 0;
    u8 handle = ti_Open(CLIPBOARD_APPVAR_NAME, "r");
    if(handle != 0) {
        u16 size = ti_GetSize(handle);
        u8 *data = ti_GetDataPtr(handle);
        insert_tokens(at, data, size);
        ti_Close(handle);
        amount_pasted = cast(s24)size;
    }
    return amount_pasted;
}

void update(void) {
    // NOTE: This will be updated after operations that affect cursor position/line breaks
    // We can afford to not update it after operations that affect cursor position but not line breaks
    // if we do that operation repeatedly and want to not iterate over linebreaks in the program much.
    s24 cursor_y = calculate_cursor_y();

    if(on_pressed) {
        editor.settings.light_mode = !editor.settings.light_mode;
        update_editor_theme_based_on_settings();
    }

    if(key_down[1] & kb_2nd) {
        if(editor.cursor_mode != CursorMode_Second) {
            editor.cursor_mode = CursorMode_Second;
        } else {
            editor.cursor_mode = CursorMode_Normal;
        }
    }
    if(key_down[2] & kb_Alpha) {
        if(editor.cursor_mode == CursorMode_Normal) {
            editor.cursor_mode = CursorMode_Alpha;
            editor.alpha_is_lowercase = false;
        } else if(editor.cursor_mode == CursorMode_Second) {
            editor.cursor_mode = CursorMode_Alpha;
            editor.alpha_is_lowercase = true;
        } else if(editor.cursor_mode == CursorMode_Alpha) {
            editor.cursor_mode = CursorMode_Normal;
        }
    }
    
    if(program.entering_goto) {
        if(key_down[6] & kb_Clear) program.entering_goto = false;
        u8 character = 0;
        if(editor.cursor_mode != CursorMode_Alpha) {
            if(key_debounced[3] & kb_1) { character = '1'; }
            if(key_debounced[4] & kb_2) { character = '2'; }
            if(key_debounced[5] & kb_3) { character = '3'; }
            if(key_debounced[3] & kb_4) { character = '4'; }
            if(key_debounced[4] & kb_5) { character = '5'; }
            if(key_debounced[5] & kb_6) { character = '6'; }
            if(key_debounced[3] & kb_7) { character = '7'; }
            if(key_debounced[4] & kb_8) { character = '8'; }
            if(key_debounced[5] & kb_9) { character = '9'; }
            if(key_debounced[3] & kb_0) { character = '0'; }
        } else {
            if(key_debounced[2] & kb_Math)   { character = 'A'; }
            if(key_debounced[3] & kb_Apps)   { character = 'B'; }
            if(key_debounced[4] & kb_Prgm)   { character = 'C'; }
            if(key_debounced[2] & kb_Recip)  { character = 'D'; }
            if(key_debounced[3] & kb_Sin)    { character = 'E'; }
            if(key_debounced[4] & kb_Cos)    { character = 'F'; }
            if(key_debounced[5] & kb_Tan)    { character = 'G'; }
            if(key_debounced[6] & kb_Power)  { character = 'H'; }
            if(key_debounced[2] & kb_Square) { character = 'I'; }
            if(key_debounced[3] & kb_Comma)  { character = 'J'; }
            if(key_debounced[4] & kb_LParen) { character = 'K'; }
            if(key_debounced[5] & kb_RParen) { character = 'L'; }
            if(key_debounced[6] & kb_Div)    { character = 'M'; }
            if(key_debounced[2] & kb_Log)    { character = 'N'; }
            if(key_debounced[3] & kb_7)      { character = 'O'; }
            if(key_debounced[4] & kb_8)      { character = 'P'; }
            if(key_debounced[5] & kb_9)      { character = 'Q'; }
            if(key_debounced[6] & kb_Mul)    { character = 'R'; }
            if(key_debounced[2] & kb_Ln)     { character = 'S'; }
            if(key_debounced[3] & kb_4)      { character = 'T'; }
            if(key_debounced[4] & kb_5)      { character = 'U'; }
            if(key_debounced[5] & kb_6)      { character = 'V'; }
            if(key_debounced[6] & kb_Sub)    { character = 'W'; }
            if(key_debounced[2] & kb_Sto)    { character = 'X'; }
            if(key_debounced[3] & kb_1)      { character = 'Y'; }
            if(key_debounced[4] & kb_2)      { character = 'Z'; }
        }
        if(key_debounced[1] & kb_Del && program.entering_goto_chars_count > 0) {
            program.entering_goto_chars_count -= 1;
        }
        if(character != 0) {
            if(program.entering_goto_chars_count <= ARRLEN(program.entering_goto_chars) - 1) {
                program.entering_goto_chars_count += 1;
                program.entering_goto_chars[program.entering_goto_chars_count - 1] = character;
            }
        }
        if(key_down[6] & kb_Enter) {
            program.entering_goto = false;
            if(program.entering_goto_chars_count > 0) {
                for(int i = 0; i <= program.size - 1;) {
                    if(program.data[i] == LBL) {
                        u8 entering_index = 0;
                        bool failed = false;
                        for(int j = i + 1; j <= min(i + 2, program.size - 1);) {
                            if((program.data[j] >= '0' && program.data[j] <= '9') ||
                               (program.data[j] >= 'A' && program.data[j] <= 'Z')) {
                                if(program.data[j] != program.entering_goto_chars[entering_index]) {
                                    failed = true;
                                    break;
                                }
                                entering_index += 1;
                            } else {
                                break;
                            }
                            j += get_token_size(j);
                        }
                        if(failed == false && entering_index == program.entering_goto_chars_count) {
                            program.cursor = i;
                            cursor_y = calculate_cursor_y();
                            program.view_top_line = max(0, cursor_y - 11);
                            break;
                        }
                    }
                    i += get_token_size(i);
                }
            }
        }
    } else if(!program.program_loaded) {
        // NOTE: Program selector
        if(key_down[6] & kb_Clear) editor.running = false;

        if(os_programs_count == 0) {
            program.selected_program = 0;
        } else if(editor.cursor_mode == CursorMode_Alpha) {
            if(key_debounced[7] & kb_Down) program.selected_program += 9;
            if(key_debounced[7] & kb_Up) program.selected_program -= 9;
            if(program.selected_program < 0) program.selected_program = 0;
            if(program.selected_program >= os_programs_count) program.selected_program = os_programs_count - 1;
        } else {
            if(key_debounced[7] & kb_Down) program.selected_program += 1;
            if(key_debounced[7] & kb_Up) program.selected_program -= 1;
            if(program.selected_program < 0) program.selected_program += os_programs_count;
            if(program.selected_program >= os_programs_count) program.selected_program -= os_programs_count;
        }

        char do_jump = 0;
        if(key_down[2] & kb_Math)  { do_jump = 'A'; } // A
        else if(key_down[3] & kb_Apps)  { do_jump = 'B'; } // B
        else if(key_down[4] & kb_Prgm)  { do_jump = 'C'; } // C
        else if(key_down[2] & kb_Recip) { do_jump = 'D'; } // D
        else if(key_down[3] & kb_Sin)   { do_jump = 'E'; } // E
        else if(key_down[4] & kb_Cos)   { do_jump = 'F'; } // F
        else if(key_down[5] & kb_Tan)   { do_jump = 'G'; } // G
        else if(key_down[6] & kb_Power) { do_jump = 'H'; } // H
        else if(key_down[2] & kb_Square){ do_jump = 'I'; } // I
        else if(key_down[3] & kb_Comma) { do_jump = 'J'; } // J
        else if(key_down[4] & kb_LParen){ do_jump = 'K'; } // K
        else if(key_down[5] & kb_RParen){ do_jump = 'L'; } // L
        else if(key_down[6] & kb_Div)   { do_jump = 'M'; } // M
        else if(key_down[2] & kb_Log)   { do_jump = 'N'; } // N
        else if(key_down[3] & kb_7)     { do_jump = 'O'; } // O
        else if(key_down[4] & kb_8)     { do_jump = 'P'; } // P
        else if(key_down[5] & kb_9)     { do_jump = 'Q'; } // Q
        else if(key_down[6] & kb_Mul)   { do_jump = 'R'; } // R
        else if(key_down[2] & kb_Ln)    { do_jump = 'S'; } // S
        else if(key_down[3] & kb_4)     { do_jump = 'T'; } // T
        else if(key_down[4] & kb_5)     { do_jump = 'U'; } // U
        else if(key_down[5] & kb_6)     { do_jump = 'V'; } // V
        else if(key_down[6] & kb_Sub)   { do_jump = 'W'; } // W
        else if(key_down[2] & kb_Sto)   { do_jump = 'X'; } // X
        else if(key_down[3] & kb_1)     { do_jump = 'Y'; } // Y
        else if(key_down[4] & kb_2)     { do_jump = 'Z'; } // Z
        if(do_jump) {
            s24 target_index = -1;
            for(int i = 0; i <= os_programs_count - 1; ++i) {
                u8 first_letter = os_programs[i].name[0];
                if(first_letter >= 'a' && first_letter <= 'z') first_letter = first_letter - 'a' + 'A';
                if(first_letter == do_jump) {
                    target_index = i;
                    break;
                }
            }
            if(target_index != -1) {
                program.selected_program = target_index;
                program.view_top_program = max(0, program.selected_program - 11);
            }
        }

        if(key_down[6] & kb_Enter && program.selected_program >= 0 && program.selected_program <= os_programs_count - 1) {
            assert(os_programs_count != 0, "Shouldn't be able to reach this logically");
            char *stored_name = (char*)os_programs[program.selected_program].name;
            load_program(stored_name);
        } else if(key_down[1] & kb_Mode) {
            u8 exists = ti_OpenVar((char*)editor.settings.last_editing_program, "r", OS_TYPE_PRGM);
            if(exists != 0) {
                ti_Close(exists);
                load_program((char*)editor.settings.last_editing_program);
                u16 cursor_y = editor.settings.last_cursor_y;
                if(cursor_y >= program.linebreaks_count) {
                    cursor_y = cast(u16)program.linebreaks_count - 1;
                }
                program.cursor = get_linebreak_location(cursor_y) + 1;
            }
        }

    } else if(program.opened_directory) {
        // NOTE: Token selector

        if((key_debounced[7] & kb_Right)) {
            program.opened_directory_list_index += 1;
            program.opened_directory_token_index = 0;
            program.opened_directory_view_top_token_index = 0;
        }
        if((key_debounced[7] & kb_Left)) {
            program.opened_directory_list_index -= 1;
            program.opened_directory_token_index = 0;
            program.opened_directory_view_top_token_index = 0;
        }
        if(program.opened_directory_list_index < 0) program.opened_directory_list_index += program.opened_directory->list_count;
        if(program.opened_directory_list_index >= program.opened_directory->list_count) program.opened_directory_list_index -= program.opened_directory->list_count;
        TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
        if(editor.cursor_mode == CursorMode_Alpha) {
            if((key_debounced[7] & kb_Up)) {
                program.opened_directory_token_index -= 8;
            }
            if((key_debounced[7] & kb_Down)) {
                program.opened_directory_token_index += 8;
            }
            if(program.opened_directory_token_index < 0) program.opened_directory_token_index = 0;
            if(program.opened_directory_token_index >= list->tokens_count) program.opened_directory_token_index = list->tokens_count - 1;
        } else {
            if((key_debounced[7] & kb_Up)) {
                program.opened_directory_token_index -= 1;
            }
            if((key_debounced[7] & kb_Down)) {
                program.opened_directory_token_index += 1;
            }
            if(program.opened_directory_token_index < 0) program.opened_directory_token_index += list->tokens_count;
            if(program.opened_directory_token_index >= list->tokens_count) program.opened_directory_token_index -= list->tokens_count;
        }
        bool select = false;
        #define TRY(index) if(index <= list->tokens_count - 1) { select = true; program.opened_directory_token_index = index; }
        if(editor.cursor_mode != CursorMode_Alpha) {
            if(key_down[3] & kb_1) { TRY(0) }
            if(key_down[4] & kb_2) { TRY(1) }
            if(key_down[5] & kb_3) { TRY(2) }
            if(key_down[3] & kb_4) { TRY(3) }
            if(key_down[4] & kb_5) { TRY(4) }
            if(key_down[5] & kb_6) { TRY(5) }
            if(key_down[3] & kb_7) { TRY(6) }
            if(key_down[4] & kb_8) { TRY(7) }
            if(key_down[5] & kb_9) { TRY(8) }
            if(key_down[3] & kb_0) { TRY(9) }
        } else {
            if(key_down[2] & kb_Math)  { TRY(10) } // A
            if(key_down[3] & kb_Apps)  { TRY(11) } // B
            if(key_down[4] & kb_Prgm)  { TRY(12) } // C
            if(key_down[2] & kb_Recip) { TRY(13) } // D
            if(key_down[3] & kb_Sin)   { TRY(14) } // E
            if(key_down[4] & kb_Cos)   { TRY(15) } // F
            if(key_down[5] & kb_Tan)   { TRY(16) } // G
            if(key_down[6] & kb_Power) { TRY(17) } // H
            if(key_down[2] & kb_Square){ TRY(18) } // I
            if(key_down[3] & kb_Comma) { TRY(19) } // J
            if(key_down[4] & kb_LParen){ TRY(20) } // K
            if(key_down[5] & kb_RParen){ TRY(21) } // L
            if(key_down[6] & kb_Div)   { TRY(22) } // M
            if(key_down[2] & kb_Log)   { TRY(23) } // N
            if(key_down[3] & kb_7)     { TRY(24) } // O
            if(key_down[4] & kb_8)     { TRY(25) } // P
            if(key_down[5] & kb_9)     { TRY(26) } // Q
            if(key_down[6] & kb_Mul)   { TRY(27) } // R
            if(key_down[2] & kb_Ln)    { TRY(28) } // S
            if(key_down[3] & kb_4)     { TRY(29) } // T
            if(key_down[4] & kb_5)     { TRY(30) } // U
            if(key_down[5] & kb_6)     { TRY(31) } // V
            if(key_down[6] & kb_Sub)   { TRY(32) } // W
            if(key_down[2] & kb_Sto)   { TRY(33) } // X
            if(key_down[3] & kb_1)     { TRY(34) } // Y
            if(key_down[4] & kb_2)     { TRY(35) } // Z
        }
        #undef TRY
        if((key_down[6] & kb_Enter) && (program.opened_directory_token_index <= list->tokens_count - 1)) select = true;
        if(select) {
            bool hardcode_a = get_is_prgm_exec_override();
            bool hardcode_b = get_is_list_name_override();
            if(hardcode_a) {
                u8 tokens[9];
                u8 tokens_count = 9;
                char *name = cast(char*)os_programs[program.opened_directory_token_index].name;
                for(u8 i = 0; i <= 8 - 1; ++i) {
                    if(name[i] == 0) {
                        tokens_count = i + 1;
                        break;
                    }
                    tokens[i + 1] = (u8)name[i];
                }
                tokens[0] = 0x5F; // prgm
                insert_tokens(program.cursor, tokens, tokens_count);
                program.cursor += tokens_count;
            } else if(hardcode_b) {
                u8 tokens[6];
                u8 tokens_count = 6;
                char *name = cast(char*)os_lists[program.opened_directory_token_index].name;
                for(u8 i = 0; i <= 5 - 1; ++i) {
                    if(name[i] == 0) {
                        tokens_count = i + 1;
                        break;
                    }
                    tokens[i + 1] = (u8)name[i];
                }
                tokens[0] = 0xEB; // L (list specifier)
                insert_tokens(program.cursor, tokens, tokens_count);
                program.cursor += tokens_count;
            } else {
                u16 token = list->tokens[program.opened_directory_token_index];

                u24 str_length = 0;
                {
                    // NOTE: Some ti_GetTokenStrings on invalid characters return like 200 for the length and are super invalid. So we just print the token number
                    // This helps people not crash their OS 5.2 when inserting tokens from OS 5.4
                    // TODO: Now, I hope that this is a consistent way to know if it's invalid. No idea. 
                    u16 *ptr = (cast(u16*)list->tokens) + program.opened_directory_token_index;
                    char *unused = ti_GetTokenString(cast(void**)&ptr, null, &str_length);
                    I_KNOW_ITS_UNUSED(unused);
                }
                if(str_length > 0 && str_length <= 50) {
                    if((token >> 8) == 0) {
                        insert_token_u8(program.cursor, cast(u8)token);
                        program.cursor += 1;
                    } else {
                        insert_token_u16(program.cursor, token);
                        program.cursor += 2;
                    }
                }
            }
            program.opened_directory = null;
        }
        if(key_down[6] & kb_Clear) program.opened_directory = null;
    } else {
        // NOTE: Editor

        if(key_down[6] & kb_Clear) editor.running = false;

        if(editor.cursor_mode == CursorMode_Normal) {
            if(key_down[4] & kb_Prgm) { open_directory(DIR_PRGM); }
            if(key_down[5] & kb_Vars) { open_directory(DIR_VARS); }
            if(key_down[2] & kb_Math) { open_directory(DIR_MATH); }
            if(key_down[4] & kb_Stat) { open_directory(DIR_STAT); }

            if(key_down[3] & kb_GraphVar) { program.entering_goto = true; program.entering_goto_chars_count = 0; }

            if(key_down[2] & kb_Recip) { insert_token_u8(program.cursor, 0x0C); program.cursor += 1; }
            if(key_down[3] & kb_Sin)   { insert_token_u8(program.cursor, 0xC2); program.cursor += 1; }
            if(key_down[4] & kb_Cos)   { insert_token_u8(program.cursor, 0xC4); program.cursor += 1; } // F
            if(key_down[5] & kb_Tan)   { insert_token_u8(program.cursor, 0xC6); program.cursor += 1; } // G
            if(key_down[6] & kb_Power) { insert_token_u8(program.cursor, 0xF0); program.cursor += 1; } // H
            if(key_down[2] & kb_Square){ insert_token_u8(program.cursor, 0x0D); program.cursor += 1; } // I
            if(key_down[3] & kb_Comma) { insert_token_u8(program.cursor, 0x2B); program.cursor += 1; } // J
            if(key_down[4] & kb_LParen){ insert_token_u8(program.cursor, 0x10); program.cursor += 1; } // K
            if(key_down[5] & kb_RParen){ insert_token_u8(program.cursor, 0x11); program.cursor += 1; } // L
            if(key_down[6] & kb_Div)   { insert_token_u8(program.cursor, 0x83); program.cursor += 1; } // M
            if(key_down[2] & kb_Log)   { insert_token_u8(program.cursor, 0xC0); program.cursor += 1; } // N
            if(key_down[3] & kb_7)     { insert_token_u8(program.cursor, 0x37); program.cursor += 1; } // O
            if(key_down[4] & kb_8)     { insert_token_u8(program.cursor, 0x38); program.cursor += 1; } // P
            if(key_down[5] & kb_9)     { insert_token_u8(program.cursor, 0x39); program.cursor += 1; } // Q
            if(key_down[6] & kb_Mul)   { insert_token_u8(program.cursor, 0x82); program.cursor += 1; } // R
            if(key_down[2] & kb_Ln)    { insert_token_u8(program.cursor, 0xBE); program.cursor += 1; } // S
            if(key_down[3] & kb_4)     { insert_token_u8(program.cursor, 0x34); program.cursor += 1; } // T
            if(key_down[4] & kb_5)     { insert_token_u8(program.cursor, 0x35); program.cursor += 1; } // U
            if(key_down[5] & kb_6)     { insert_token_u8(program.cursor, 0x36); program.cursor += 1; } // V
            if(key_down[6] & kb_Sub)   { insert_token_u8(program.cursor, 0x71); program.cursor += 1; } // W
            if(key_down[2] & kb_Sto)   { insert_token_u8(program.cursor, 0x04); program.cursor += 1; } // X
            if(key_down[3] & kb_1)     { insert_token_u8(program.cursor, 0x31); program.cursor += 1; } // Y
            if(key_down[4] & kb_2)     { insert_token_u8(program.cursor, 0x32); program.cursor += 1; } // Z
            if(key_down[5] & kb_3)     { insert_token_u8(program.cursor, 0x33); program.cursor += 1; }
            if(key_down[6] & kb_Add)   { insert_token_u8(program.cursor, 0x70); program.cursor += 1; }
            if(key_down[3] & kb_0)     { insert_token_u8(program.cursor, 0x30); program.cursor += 1; }
            if(key_down[4] & kb_DecPnt){ insert_token_u8(program.cursor, 0x3A); program.cursor += 1; }
            if(key_down[5] & kb_Chs)   { insert_token_u8(program.cursor, 0xB0); program.cursor += 1; }

#if 0
            if(key_down[3] & kb_Apps) {
                // TODO: I disabled this because of this bug
                // https://github.com/CE-Programming/toolchain/issues/459
                // According to some people, there's a work-around
                // that is creating a "parser hook" and providing different behavior on the `Stop` token?
                // But it will take time to research that, so I'll see if the bug gets fixed upstream first
                // and work on other parts of the editor.
                // https://wikiti.brandonw.net/index.php?title=83Plus:Hooks:9BAC
                editor.running = false;
                editor.run_program_at_end = true;
            }
#endif
        }
        else if(editor.cursor_mode == CursorMode_Alpha) {
            if(key_down[2] & kb_Math)  { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB0BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'A'); program.cursor += 1; } } // A
            if(key_down[3] & kb_Apps)  { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB1BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'B'); program.cursor += 1; } } // B
            if(key_down[4] & kb_Prgm)  { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB2BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'C'); program.cursor += 1; } } // C
            if(key_down[2] & kb_Recip) { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB3BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'D'); program.cursor += 1; } } // D
            if(key_down[3] & kb_Sin)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB4BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'E'); program.cursor += 1; } } // E
            if(key_down[4] & kb_Cos)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB5BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'F'); program.cursor += 1; } } // F
            if(key_down[5] & kb_Tan)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB6BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'G'); program.cursor += 1; } } // G
            if(key_down[6] & kb_Power) { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB7BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'H'); program.cursor += 1; } } // H
            if(key_down[2] & kb_Square){ if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB8BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'I'); program.cursor += 1; } } // I
            if(key_down[3] & kb_Comma) { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB9BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'J'); program.cursor += 1; } } // J
            if(key_down[4] & kb_LParen){ if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBABB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'K'); program.cursor += 1; } } // K
            if(key_down[5] & kb_RParen){ if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBCBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'L'); program.cursor += 1; } } // L
            if(key_down[6] & kb_Div)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBDBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'M'); program.cursor += 1; } } // M
            if(key_down[2] & kb_Log)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBEBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'N'); program.cursor += 1; } } // N
            if(key_down[3] & kb_7)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBFBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'O'); program.cursor += 1; } } // O
            if(key_down[4] & kb_8)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC0BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'P'); program.cursor += 1; } } // P
            if(key_down[5] & kb_9)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC1BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Q'); program.cursor += 1; } } // Q
            if(key_down[6] & kb_Mul)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC2BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'R'); program.cursor += 1; } } // R
            if(key_down[2] & kb_Ln)    { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC3BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'S'); program.cursor += 1; } } // S
            if(key_down[3] & kb_4)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC4BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'T'); program.cursor += 1; } } // T
            if(key_down[4] & kb_5)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC5BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'U'); program.cursor += 1; } } // U
            if(key_down[5] & kb_6)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC6BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'V'); program.cursor += 1; } } // V
            if(key_down[6] & kb_Sub)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC7BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'W'); program.cursor += 1; } } // W
            if(key_down[2] & kb_Sto)   { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC8BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'X'); program.cursor += 1; } } // X
            if(key_down[3] & kb_1)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC9BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Y'); program.cursor += 1; } } // Y
            if(key_down[4] & kb_2)     { if(editor.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xCABB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Z'); program.cursor += 1; } } // Z
            if(key_down[5] & kb_3)     { insert_token_u8(program.cursor, 0x5B); program.cursor += 1; }
            if(key_down[6] & kb_Add)   { insert_token_u8(program.cursor, 0x2A); program.cursor += 1; }
            if(key_down[3] & kb_0)     { insert_token_u8(program.cursor, SPACE); program.cursor += 1; }
            if(key_down[4] & kb_DecPnt){ insert_token_u8(program.cursor, 0x3E); program.cursor += 1; }
            if(key_down[5] & kb_Chs)   { insert_token_u8(program.cursor, 0xAF); program.cursor += 1; }
        } else if(editor.cursor_mode == CursorMode_Second) {
            if(key_down[2] & kb_Math ) { open_directory(DIR_TEST); }
            if(key_down[2] & kb_Recip) { open_directory(DIR_MATRIX); }
            if(key_down[3] & kb_0    ) { open_directory(DIR_ALL); }
            if(key_down[4] & kb_Prgm ) { open_directory(DIR_DRAW); }
            if(key_down[3] & kb_Apps ) { open_directory(DIR_ANGLE); }
            if(key_down[5] & kb_Vars ) { open_directory(DIR_DISTR); }
            if(key_down[4] & kb_Stat ) { open_directory(DIR_LIST); }

            #define M(prefix,token) (u16)((((u16)token) << 8) | (u16)prefix)
            if(key_down[3] & kb_Sin)   { insert_token_u8(program.cursor, OS_TOK_INV_SIN); program.cursor += 1; }
            if(key_down[4] & kb_Cos)   { insert_token_u8(program.cursor, OS_TOK_INV_COS); program.cursor += 1; }
            if(key_down[5] & kb_Tan)   { insert_token_u8(program.cursor, OS_TOK_INV_TAN); program.cursor += 1; }
            if(key_down[6] & kb_Power) { insert_token_u8(program.cursor, OS_TOK_PI); program.cursor += 1; }
            if(key_down[2] & kb_Square){ insert_token_u8(program.cursor, OS_TOK_SQRT); program.cursor += 1; }
            if(key_down[3] & kb_Comma) { insert_token_u8(program.cursor, OS_TOK_EXP_10); program.cursor += 1; }
            if(key_down[4] & kb_LParen){ insert_token_u8(program.cursor, OS_TOK_LEFT_BRACE); program.cursor += 1; }
            if(key_down[5] & kb_RParen){ insert_token_u8(program.cursor, OS_TOK_RIGHT_BRACE); program.cursor += 1; }
            if(key_down[6] & kb_Div)   { insert_token_u16(program.cursor, 0x31BB); program.cursor += 2; } // euler's constant
            if(key_down[2] & kb_Log)   { insert_token_u8(program.cursor, OS_TOK_INV_LOG); program.cursor += 1; }
            if(key_down[3] & kb_7)     { insert_token_u16(program.cursor, M(OS_TOK_EQU, OS_TOK_EQU_U)); program.cursor += 2; }
            if(key_down[4] & kb_8)     { insert_token_u16(program.cursor, M(OS_TOK_EQU, OS_TOK_EQU_V)); program.cursor += 2; }
            if(key_down[5] & kb_9)     { insert_token_u16(program.cursor, M(OS_TOK_EQU, OS_TOK_EQU_W)); program.cursor += 2; }
            if(key_down[6] & kb_Mul)   { insert_token_u8(program.cursor, OS_TOK_LEFT_BRACKET); program.cursor += 1; }
            if(key_down[2] & kb_Ln)    { insert_token_u8(program.cursor, 0xBF); program.cursor += 1; }
            if(key_down[3] & kb_4)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L4)); program.cursor += 2; }
            if(key_down[4] & kb_5)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L5)); program.cursor += 2; }
            if(key_down[5] & kb_6)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L6)); program.cursor += 2; }
            if(key_down[6] & kb_Sub)   { insert_token_u8(program.cursor, OS_TOK_RIGHT_BRACKET); program.cursor += 1; }
            // NOTE: Our own custom behavior for 2ND->STO
            if(key_down[2] & kb_Sto)   { insert_token_u8(program.cursor, OS_TOK_LIST_L); program.cursor += 1; }
            if(key_down[3] & kb_1)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L1)); program.cursor += 2; }
            if(key_down[4] & kb_2)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L2)); program.cursor += 2; }
            if(key_down[5] & kb_3)     { insert_token_u16(program.cursor, M(OS_TOK_LIST, OS_TOK_LIST_L3)); program.cursor += 2; }
            // if(key_down[6] & kb_Add)   { insert_token_u8(program.cursor, ); program.cursor += 1; }
            if(key_down[4] & kb_DecPnt){ insert_token_u8(program.cursor, 0x2C); program.cursor += 1; } // complex i
            if(key_down[5] & kb_Chs)   { insert_token_u8(program.cursor, 0x72); program.cursor += 1; }
            #undef M
        }
        
        if(key_down[1] & kb_Graph) {
            Delta* undo = pop_delta(&program.undo_buffer);
            if(undo) {
                apply_delta_to_program(undo, &program.redo_buffer);
                cursor_y = calculate_cursor_y();
            }
        }
        
        if(key_down[1] & kb_Trace) {
            Delta* redo = pop_delta(&program.redo_buffer);
            if(redo) {
                apply_delta_to_program(redo, &program.undo_buffer);
                cursor_y = calculate_cursor_y();
            }
        }
        
        if(key_down[1] & kb_Yequ) {
            // NOTE: Toggle selection
            program.cursor_selecting = !program.cursor_selecting;
            program.cursor_started_selecting = program.cursor;
        }
        if(key_down[1] & kb_Window) {
            if(program.cursor_selecting) {
                // Copy
                Range range = get_selecting_range();
                s24 size = (range.max + 1) - range.min;
                assert(size >= 0, "Underflow error");
                if(save_clipboard(range.min, size)) {
                    program.cursor_selecting = false;
                }
            } else {
                // Paste without moving cursor
                paste_clipboard(program.cursor);
            }
            cursor_y = calculate_cursor_y();
        }
        if(key_down[1] & kb_Zoom) {
            if(program.cursor_selecting) {
                // Cut
                Range range = get_selecting_range();
                s24 size = (range.max + 1) - range.min;
                assert(size >= 0, "Underflow error");
                if(save_clipboard(range.min, size)) {
                    program.cursor_selecting = false;
                    remove_tokens(range.min, (u16)size);
                    program.cursor = range.min;
                }
            } else {
                // Paste with moving cursor
                s24 amount_pasted = paste_clipboard(program.cursor);
                program.cursor += amount_pasted;
            }
            cursor_y = calculate_cursor_y();
        }
        if(key_debounced[1] & kb_Del) {
            if(program.cursor_selecting) {
                Range range = get_selecting_range();
                s24 size = (range.max + 1) - range.min;
                if(size >= 0 && size <= 65536) {
                    remove_tokens(range.min, cast(u16)size);
                    program.cursor = range.min;
                    program.cursor_selecting = false;
                } else {
                    assert(size >= 0 && size <= 65536, "Must be within u16 range");
                }
            } else {
                if(program.cursor <= program.size - 1) {
                    remove_tokens(program.cursor, get_token_size(program.cursor));
                }
            }
            cursor_y = calculate_cursor_y();
        }
        if(key_held[1] & kb_Mode) {
            // NOTE: Clear line
            s24 line_start = get_linebreak_location(cursor_y) + 1;
            s24 before_line_break_or_end_of_program = program.size - 1;
            for(s24 i = line_start; i <= program.size - 1;) {
                if(program.data[i] == LINEBREAK) {
                    before_line_break_or_end_of_program = i - 1;
                    break;
                }
                i += get_token_size(i);
            }
            if(before_line_break_or_end_of_program >= line_start) {
                s24 size = (before_line_break_or_end_of_program + 1) - line_start;
                if(size >= 1 && size < 65535) {
                    remove_tokens(line_start, cast(u16)size);
                    program.cursor = line_start;
                    cursor_y = calculate_cursor_y();
                } else {
                    assert(size >= 1 && size < 65535, "Must be within u16 range");
                }
            }
        }
        if(key_debounced[6] & kb_Enter) {
            insert_token_u8(program.cursor, LINEBREAK);
            program.cursor += 1;
        }
        if(key_debounced[7] & kb_Down) {
            s24 target_cursor = cursor_y;
            if(editor.cursor_mode == CursorMode_Alpha) target_cursor += 8;
            else target_cursor += 1;
            if(target_cursor >= program.linebreaks_count - 1) target_cursor = program.linebreaks_count - 1;
            program.cursor = get_linebreak_location(target_cursor) + 1;
        }
        if(key_debounced[7] & kb_Up) {
            s24 target_cursor = cursor_y;
            if(editor.cursor_mode == CursorMode_Alpha) target_cursor -= 8;
            else target_cursor -= 1;
            if(target_cursor < 0) target_cursor = 0;
            program.cursor = get_linebreak_location(target_cursor) + 1;
        }
        if(key_debounced[7] & kb_Left) {
            if(editor.cursor_mode == CursorMode_Second) {
                program.cursor = get_linebreak_location(cursor_y) + 1;
            } else {
                if(program.cursor > 0) {
                    u8 prev_token_size = get_token_size(program.cursor - 1);
                    program.cursor -= prev_token_size;
                }
            }
        }
        if(key_debounced[7] & kb_Right) {
            if(editor.cursor_mode == CursorMode_Second) {
                s24 target = program.size;
                if(cursor_y <= program.linebreaks_count - 2) { target = get_linebreak_location(cursor_y + 1); }
                program.cursor = target;
            } else {
                u8 size = get_token_size(program.cursor);
                program.cursor += size;
                if(program.cursor >= program.size) {
                    program.cursor = program.size;
                }
            }
        }
    }
}

Range get_selecting_range() {
    assert(program.cursor_selecting, "This function should only be called if we're selecting. Otherwise you're probably doing something wrong.");
    Range result;
    if(program.cursor < program.cursor_started_selecting) {
        result.min = program.cursor;
        result.max = program.cursor_started_selecting + (get_token_size(program.cursor_started_selecting) - 1);
    } else {
        result.min = program.cursor_started_selecting;
        result.max = program.cursor + (get_token_size(program.cursor) - 1);
    }
    assert(result.min <= result.max, "Weird");
    return result;
}

// NOTE: Always either 1 or 2.
// Can pass a position that's on the first or second byte of a token
u8 get_token_size(s24 pos) {
    u8 result = 1;
    u8 x = program.data[pos];
    #define IS_TWOBYTE(x) \
        (x == 0x5C || x == 0x5D || x == 0x5E || x == 0x60 || x == 0x61 || x == 0x62 || x == 0x63 || x == 0xAA || x == 0x7B || x == 0xBB || x == 0xEF)
    if(IS_TWOBYTE(x)) {
        result = 2;
    }
    if(result == 1 && pos > 0) {
        u8 x = program.data[pos - 1];
        if(IS_TWOBYTE(x)) {
            result = 2;
        }
    }
    return result;
}

char hexes[] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
// NOTE: Result should be two bytes long
void convert_byte_to_hex(u8 input, char *result) {
    result[0] = hexes[input/16];
    result[1] = hexes[input & (16 - 1)];
}

void blit_loading_indicator(void) {
    gfx_BlitScreen();

    gfx_SetColor(editor.background_color);
    #define LOADING_INDICATOR_WIDTH 40
    fontlib_SetForegroundColor(editor.foreground_color);
    fontlib_SetBackgroundColor(editor.background_color);
    fontlib_SetTransparency(true);
    gfx_FillRectangle_NoClip((320/2)-(LOADING_INDICATOR_WIDTH/2), 0, LOADING_INDICATOR_WIDTH, FONT_HEIGHT + 2);
    draw_string("...", (320/2) - ((FONT_WIDTH*3)/2), 1);
    #undef LOADING_INDICATOR_WIDTH

    gfx_SwapDraw();
}

void render(void) {

    gfx_FillScreen(editor.background_color);
    fontlib_SetForegroundColor(editor.foreground_color);
    fontlib_SetBackgroundColor(editor.background_color);
    fontlib_SetTransparency(true);
    fontlib_SetFirstPrintableCodePoint(0);
    gfx_SetColor(editor.foreground_color);

    if(!program.program_loaded) {
        const u8 COUNT_PER_SCREEN = 22;
        if(program.selected_program - program.view_top_program >= COUNT_PER_SCREEN) {
            program.view_top_program = program.selected_program - COUNT_PER_SCREEN;
        }
        if(program.view_top_program > program.selected_program) {
            program.view_top_program = program.selected_program;
        }

        u24 x = 320/2 - FONT_WIDTH*4;
        u8 y = 5;
        for(int i = program.view_top_program; y < 230 && i < os_programs_count; ++i) {
            bool selected = (i == program.selected_program);
            if(selected) {
                fontlib_SetForegroundColor(editor.background_color);
                gfx_SetColor(editor.foreground_color);
                gfx_FillRectangle_NoClip(x - 1, y - 1, FONT_WIDTH*8 + 2, FONT_HEIGHT + 2);
            } else {
                fontlib_SetForegroundColor(editor.foreground_color);
            }
            char *name = (char*)os_programs[i].name;
            draw_string_max_chars(name, 8, x, y);

            bool archived = false;
            u24 size_bar_width = 0;
            u8 temp_handle = ti_OpenVar(name, "r", OS_TYPE_PRGM);
            if(temp_handle) {
                archived = (ti_IsArchived(temp_handle) != 0);
                u24 size = ti_GetSize(temp_handle);
                ti_Close(temp_handle);
                const u24 max_width = x - 20;
                size_bar_width = ((size * max_width) / 65535);
            }
            if(archived) { gfx_SetColor(0xD5); }
            else { gfx_SetColor(editor.foreground_color); }
            gfx_FillRectangle_NoClip(x + FONT_WIDTH*8 + 10, y, size_bar_width, FONT_HEIGHT);

            if(archived) {
                fontlib_SetForegroundColor(editor.foreground_color);
                draw_string("*", x - (FONT_WIDTH + 2), y + 3);
            }

            y += FONT_HEIGHT + 2;
        }

    } else if(program.opened_directory) {
        const u8 COUNT_PER_SCREEN = 21;
        if(program.opened_directory_token_index - program.opened_directory_view_top_token_index >= COUNT_PER_SCREEN) {
            program.opened_directory_view_top_token_index = program.opened_directory_token_index - COUNT_PER_SCREEN;
        }
        if(program.opened_directory_view_top_token_index > program.opened_directory_token_index) {
            program.opened_directory_view_top_token_index = program.opened_directory_token_index;
        }

        u24 x = 5;
        for(int i = 0; i < program.opened_directory->list_count; ++i) {
            bool selected = (i == program.opened_directory_list_index);
            u24 width = (FONT_WIDTH*cast(u24)program.opened_directory->lists[i].name_count);
            if(!selected) {
                fontlib_SetForegroundColor(editor.foreground_color);
            } else {
                fontlib_SetForegroundColor(editor.background_color);
                gfx_SetColor(editor.foreground_color);
                gfx_FillRectangle_NoClip(x - 1, 5, width + 2, FONT_HEIGHT + 1);
            }
            draw_string(program.opened_directory->lists[i].name, cast(u24)x, 5);
            x += width + 4;
        }
        TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
        u8 y = 15;
        bool is_prgm_exec_hardcode_override = get_is_prgm_exec_override();
        bool is_list_name_hardcode_override = get_is_list_name_override();
        for(u24 i = cast(u24)program.opened_directory_view_top_token_index; i < cast(u24)list->tokens_count && i < cast(u24)program.opened_directory_view_top_token_index + 22; ++i) {
            u24 str_length = 0;
            char *str;
            if(is_prgm_exec_hardcode_override) {
                str = (char*)os_programs[i].name;
                str_length = 8;
                for(int i = 0; i <= 7; ++i) {
                    if(str[i] == 0) {
                        str_length = (u24)i;
                        break;
                    }
                }
            } else if(is_list_name_hardcode_override) {
                str = (char*)os_lists[i].name;
                str_length = 5;
                for(int i = 0; i <= 5 - 1; ++i) {
                    if(str[i] == 0) {
                        str_length = (u24)i;
                        break;
                    }
                }
            } else {
                u16 *ptr = (cast(u16*)list->tokens) + i;
                str = ti_GetTokenString(cast(void**)&ptr, null, &str_length);
            }

            if(str_length == 0 || str_length >= 40) {
                // NOTE: Some ti_GetTokenStrings on invalid characters return like 200 for the length and are super invalid. So we just print the token number
                static u8 number_storage[10] = "(xxxx)";
                u16 num = list->tokens[i];
                convert_byte_to_hex(cast(u8)(num >> 8), (char*)&number_storage[1]);
                convert_byte_to_hex(cast(u8)(num), (char*)&number_storage[3]);
                str = cast(char*)number_storage;
                str_length = 6;
            }
            
            bool selected = (cast(s24)i == program.opened_directory_token_index);
            u24 width = (str_length+2)*FONT_WIDTH;
            if(!selected) {
                fontlib_SetForegroundColor(editor.foreground_color);
            } else {
                fontlib_SetForegroundColor(editor.background_color);
                gfx_SetColor(editor.foreground_color);
                gfx_FillRectangle_NoClip(4, y - 1, width + 4, FONT_HEIGHT + 2);
            }

            char *key_table[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
            char *key = null;
            if(i >= 0 && i <= ARRLEN(key_table) - 1) { key = key_table[i]; }
            else { key = null; }
            if(key) { draw_string(key, 5, y); }
            draw_string_max_chars(str, str_length, 2*FONT_WIDTH+7, y);
            y += FONT_HEIGHT + 2;
        }
        fontlib_SetForegroundColor(editor.foreground_color);
    } else {
        s24 cursor_y = program.linebreaks_count - 1;
        for(int i = 0; i <= program.linebreaks_count - 1; ++i) {
            if(program.cursor <= get_linebreak_location(i)) {
                cursor_y = i - 1;
                break;
            }
        }

        int x = 5;
        u8 y = 5;
        s24 max_width = 320 - (FONT_WIDTH+2);
        s24 max_height = 240 - FONT_HEIGHT;
        s24 chars_per_line = max_width/FONT_WIDTH;
        s24 lines_per_screen = (max_height / (FONT_HEIGHT + 2));

        if(cursor_y < program.view_top_line) {
            program.view_top_line = cursor_y;
        }
        if(cursor_y >= program.view_top_line + (lines_per_screen - 1)) {
            program.view_top_line = cursor_y - (lines_per_screen - 1);
        }

        s24 bottom_line = min(program.linebreaks_count - 1, program.view_top_line + lines_per_screen);
        if(program.linebreaks_dirty_indentation_min <= bottom_line) {
            assert(program.linebreaks_dirty_indentation_min != 0, "How did line 0 get dirty?");
            if(program.linebreaks_dirty_indentation_min != 0) {
                s24 indentation = program.linebreaks[program.linebreaks_dirty_indentation_min - 1].indentation;
                for(int i = program.linebreaks_dirty_indentation_min - 1; i <= bottom_line; ++i) {
                    program.linebreaks[i].indentation = cast(u8)indentation;
                    s24 first_loc = get_linebreak_location(i)+1;
                    s24 second_loc = get_linebreak_location(i+1)-1;
                    u8 byte = program.data[first_loc];
                    change_indentation_based_on_byte(indentation, byte);
                    if(second_loc != first_loc) {
                        byte = program.data[second_loc];
                        change_indentation_based_on_byte(indentation, byte);
                    }
                }
                program.linebreaks_dirty_indentation_min = cast(u16)bottom_line + 1;
            }
        }

        s24 cursor_char_in_line = 0;
        {
            s24 last_char_in_line = program.size - 1;
            if(cursor_y + 1 <= program.linebreaks_count - 1) { last_char_in_line = get_linebreak_location(cursor_y + 1) - 1; }
            for(int i = get_linebreak_location(cursor_y) + 1; i < last_char_in_line;) {
                u8 byte_0 = program.data[i];
                assert(byte_0 != LINEBREAK, "Impossible? Linebreak pointers misaligned?");
                u24 str_length = 1;
                if(byte_0 != SPACE) {
                    u8 *ptr = program.data + i;
                    char *unused = ti_GetTokenString(cast(void**)&ptr, null, &str_length);
                    I_KNOW_ITS_UNUSED(unused);
                }
                if(i == program.cursor) {
                    break;
                }
                cursor_char_in_line += str_length;
                i += get_token_size(i);
            }
        }
        cursor_char_in_line += program.linebreaks[cursor_y].indentation;
        if(cursor_char_in_line <= chars_per_line - 2) {
            program.view_first_character = 0;
        } else {
            program.view_first_character = (cursor_char_in_line - cast(s24)chars_per_line/cast(s24)2);
        }

        int first_token = get_linebreak_location(program.view_top_line) + 1;
        s24 current_view_y = program.view_top_line;
        Range selection;
        if(program.cursor_selecting) selection = get_selecting_range();
        s24 chars_until_line = -program.view_first_character;
        s24 indentation_level = program.linebreaks[program.view_top_line].indentation;
        if(chars_until_line < 0) {
            s24 change = min(-chars_until_line, indentation_level);
            chars_until_line += change;
            indentation_level -= change;
        }
        x += indentation_level * FONT_WIDTH;

        // NOTE: On a stress test (DJ Omnimaga's pokewalrus data files), frame time is
        // 13ms with nothing, 115ms with updating, 230ms with updating/rendering.
        for(int i = first_token; i < program.size;) {
            bool on_cursor = (i == program.cursor);
            bool selected;
            if(program.cursor_selecting) {
                selected = i >= selection.min && i <= selection.max;
            } else {
                selected = on_cursor;
            }
            s24 str_length = 0;
            char *str;
            u8 byte_0 = program.data[i];
            u8 token_size = get_token_size(i);
            
            if(byte_0 != LINEBREAK && byte_0 != SPACE) {
                u8 *ptr = program.data + i;
                u24 str_length_unsigned;
                str = ti_GetTokenString(cast(void**)&ptr, null, &str_length_unsigned);
                str_length = cast(s24)str_length_unsigned;
            }
            
            i += token_size;
            
            bool break_line = false;
            
            if(chars_until_line < 0 && str_length > 0) {
                s24 amount_to_move = min(str_length, -chars_until_line);
                str_length -= amount_to_move;
                str += amount_to_move;
                chars_until_line += amount_to_move;
            }

            if(str_length > 0) {
                if(selected) { fontlib_SetTransparency(false); fontlib_SetBackgroundColor(editor.highlight_color); }
                else { fontlib_SetTransparency(true); }
                s24 start = x;
                s24 end = x + cast(s24)str_length*FONT_WIDTH;
                s24 max_chars = (min(max_width, end) - start) / FONT_WIDTH;
                if(end > max_width) {
                    break_line = true;
                }
                if(max_chars > 0) {
                    draw_string_max_chars(str,(u24)max_chars, (u24)x,y);
                }
            } else if(selected && (byte_0 == LINEBREAK || byte_0 == SPACE)) {
                u24 length_of_rect;
                if(byte_0 == LINEBREAK) { length_of_rect = FONT_WIDTH/2; }
                if(byte_0 == SPACE) { length_of_rect = FONT_WIDTH; }
                gfx_SetColor(editor.highlight_color);
                gfx_FillRectangle_NoClip(cast(u24)x,y,length_of_rect,FONT_HEIGHT);
                gfx_SetColor(editor.foreground_color);
            }

            if(on_cursor && !program.cursor_selecting) {
                u8 length_of_rect = FONT_WIDTH;
                if(str_length > 0) { length_of_rect = cast(u8)str_length*FONT_WIDTH; }
                gfx_FillRectangle_NoClip(cast(u24)x,y+FONT_HEIGHT+1,length_of_rect,1);
                gfx_FillRectangle_NoClip(cast(u24)x-1,y,1,FONT_HEIGHT);
            }
            if(str_length > 0) {
                x += str_length*FONT_WIDTH;
            }
            if(byte_0 == SPACE) { if(chars_until_line < 0) { chars_until_line += 1; } else { x += FONT_WIDTH; } }
            if(byte_0 == 0x3F) { break_line = true; }
            
            if(break_line) {
                x = 5;
                y += FONT_HEIGHT + 2;
                current_view_y += 1;
                chars_until_line = -program.view_first_character;
                indentation_level = program.linebreaks[current_view_y].indentation;
                if(chars_until_line < 0) {
                    s24 change = min(-chars_until_line, indentation_level);
                    chars_until_line += change;
                    indentation_level -= change;
                }
                x += indentation_level * FONT_WIDTH;
                if(current_view_y <= program.linebreaks_count - 1) {
                    i = get_linebreak_location(current_view_y) + 1;
                    if(current_view_y == program.linebreaks_count - 1) {
                        // NOTE: Bottom line indicating end of file
                        gfx_FillRectangle(0,y+FONT_HEIGHT+4,64,1);
                    }
                } else {
                    break;
                }
            }
            if(y >= max_height)
            {
                break;
            }
        }
        if(program.cursor >= program.size) {
            gfx_FillRectangle_NoClip(cast(u24)x,y+FONT_HEIGHT+1,FONT_WIDTH,3);
            gfx_FillRectangle_NoClip(cast(u24)x-1,y,2,FONT_HEIGHT);
        }

        s24 scrollbar_target_y = ((cast(s24)cursor_y * 230) / (cast(s24)program.linebreaks_count - 1));
        // NOTE: Lerp is x + (y-x)*a;
        program.scroller_visual_y = program.scroller_visual_y + (((scrollbar_target_y - program.scroller_visual_y) * 3) / 10);
        gfx_FillRectangle_NoClip(320-2, cast(u8)program.scroller_visual_y, 2, 10);
        
        s24 undo_bar_target_height = cast(s24)min(240, 3*program.undo_buffer.delta_count);
        program.undo_bar_visual_height = program.undo_bar_visual_height + (((undo_bar_target_height - program.undo_bar_visual_height) * 6) / 10);
        if(program.undo_bar_visual_height <= 3 && undo_bar_target_height == 0) { program.undo_bar_visual_height = 0; }
        gfx_FillRectangle_NoClip(320-5,240-cast(u8)program.undo_bar_visual_height,2,cast(u8)program.undo_bar_visual_height);

        s24 redo_bar_target_height = cast(s24)min(240, 3*program.redo_buffer.delta_count);
        program.redo_bar_visual_height = program.redo_bar_visual_height + (((redo_bar_target_height - program.redo_bar_visual_height) * 6) / 10);
        if(program.redo_bar_visual_height <= 3 && redo_bar_target_height == 0) { program.redo_bar_visual_height = 0; }
        gfx_FillRectangle_NoClip(320-8,240-cast(u8)program.redo_bar_visual_height,2,cast(u8)program.redo_bar_visual_height);
        
        // log("%2x %2x [%2x] %2x %2x\n", program.data[program.cursor - 2], program.data[program.cursor - 1], program.data[program.cursor], program.data[program.cursor + 1], program.data[program.cursor+2]);
    }
    
    fontlib_SetTransparency(true);
    fontlib_SetForegroundColor(editor.foreground_color);
    char *cursor_glyph = null;
    if(editor.cursor_mode == CursorMode_Second) {
        cursor_glyph = "\xE1";
    } else if(editor.cursor_mode == CursorMode_Alpha) {
        if(editor.alpha_is_lowercase) {
            cursor_glyph = "\xE3";
        } else {
            cursor_glyph = "\xE2";
        }
    }
    if(cursor_glyph) {
        draw_string(cursor_glyph, 320-(FONT_WIDTH+8), 2);
    }

    if(program.entering_goto) {
        fontlib_SetTransparency(true);
        fontlib_SetForegroundColor(editor.foreground_color);
        u24 rect_min_x = 160 - 40;
        u24 rect_max_x = 160 + 40;
        u8 rect_min_y = 120 - 20;
        u8 rect_max_y = 120 + 20;
        u24 rect_width = rect_max_x - rect_min_x;
        u8 rect_height = rect_max_y - rect_min_y;
        gfx_SetColor(editor.background_color);
        gfx_FillRectangle_NoClip(rect_min_x-2, rect_min_y-2, rect_width+4, rect_height+4);
        gfx_SetColor(editor.foreground_color);
        gfx_Rectangle_NoClip(rect_min_x, rect_min_y, rect_width, rect_height);
        draw_string_max_chars((char*)program.entering_goto_chars, program.entering_goto_chars_count,
                              rect_min_x + 15, 240/2 - FONT_HEIGHT/2);
    }
}

#if DEBUG
void dump_deltas(char *title) {
    log("\n\n==+== %s\nUndo:\n", title);
    Delta *it = (Delta*)program.undo_buffer.data;
    for(u24 i = 0; i < program.undo_buffer.delta_count; ++i) {
        dump_delta("", it);
        it = cast(Delta*)(cast(u8*)it + size_of_delta(it));
    }
    
    log("\nRedo:\n");
    it = (Delta*)program.redo_buffer.data;
    for(u24 i = 0; i < program.redo_buffer.delta_count; ++i) {
        dump_delta("", it);
        it = cast(Delta*)(cast(u8*)it + size_of_delta(it));
    }
}
void dump(char *dump_name) {
    log("\n%s:\n", dump_name);
    assert(program.linebreaks[0].location_ == 0, "First linebreak should be undestructed");
    int current_linebreak = 1;
    for(int i = 0; i <= program.size - 1; ++i) {
        bool is_linebreak = false;
        if(current_linebreak <= program.linebreaks_count - 1) {
            if(get_linebreak_location(current_linebreak) == i) {
                is_linebreak = true;
                current_linebreak += 1;
            }
        }
        if(is_linebreak) {
            log("[");
        }
        log("%2x", program.data[i]);
        if(is_linebreak) {
            log("]");
        }
        
    }
    log("\n%d linebreaks\n", program.linebreaks_count);
    for(int i = 0; i < program.linebreaks_count; ++i) {
        log("%d,", program.linebreaks[i].location_);
    }
    log("\n");
}
void dump_delta(char *title, Delta *delta) {
    log("\nDump delta %s\n", title);
    if(delta->type == Delta_InsertTokens) {
        log("Insert tokens\n-Cursor %d\n-At %d\n-Count %d\n", delta->cursor_was, delta->insert_data.at, delta->insert_data.count);
    } else {
        log("Remove tokens\n-Cursor %d\n-At %d\n-Count %d\nData:\n", delta->cursor_was, delta->remove_data.at, delta->remove_data.count);
        u8 *data = cast(u8*)(delta + 1);
        for(int i = 0; i < delta->remove_data.count; ++i) {
            log("%2x ", data[i]);
        }
        log("\n");
    }
}
#endif
#include <fileioc.h>
#include <ti/screen.h>
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

void copy(void *src, void *dest, s24 count);
void zero(void *dest, s24 count);
void update(void);
void render(void);
void update_input(void);
u8 get_token_size(s24 position_in_program);
typedef struct Range { s24 min; s24 max; } Range;
Range get_selecting_range();

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
#else
#define log(...)
#define assert(...)
#define dump(...)
#endif

#define BACKGROUND 0x08
#define FOREGROUND 0xF7
#define HIGHLIGHT 0x7A

#define LINEBREAK 0x3F
#define SPACE 0x29

#define FONT_WIDTH 7
#define FONT_HEIGHT 8

#define ARRLEN(var) (sizeof((var)) / sizeof((var)[0]))

typedef struct TokenList {
    char *name;
    s16 name_count;
    u16 *tokens;
    s16  tokens_count;
} TokenList;

typedef struct TokenDirectory {
    s8 list_count;
    TokenList lists[5];
} TokenDirectory;

// TODO: Tokens that I don't know the ID of (may be from new OS's not in old tables)
// (aka IDK)
// `Wait` `GraphColor(` `OpenLib(` `ExecLib`
u16 CTRL[] = { 0xCE, 0xCF, 0xD0, 0xD3, 0xD1, 0xD2, 0xD4, 0xD8, 0xD6,
               0xD7, 0xDA, 0xDB, 0xE6, 0x5F, 0xD5, 0xD9, 0x54BB, 0x45BB };
// TODO: IDK `eval(` `toString(`
u16 IO[]   = { 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE0, 0xAD, 0xE1, 0xFB,
               0x53BB, 0xE8, 0xE7, 0x2ABB, 0x56BB };
// TODO: IDK any of the COLOR things in prgm
u16 COLOR[] = {};
#define DIR_PRGM 0
TokenDirectory directories[] = {
    { .list_count = 4, .lists = {
        { .name = "CTRL" , .name_count = 4, .tokens = CTRL , .tokens_count = ARRLEN(CTRL) },
        { .name = "I/O"  , .name_count = 3, .tokens = IO   , .tokens_count = ARRLEN(IO) },
        { .name = "COLOR", .name_count = 5, .tokens = COLOR, .tokens_count = ARRLEN(COLOR) },
        // NOTE: This gets hardcoded override treatment
        { .name = "EXEC" , .name_count = 4, .tokens = null , .tokens_count = 0 },
    } }
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


#if DEBUG
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
void dump_deltas(char *title); // NOTE: forward declared... C is ugly
#else
#define dump_delta(...)
#define dump_deltas(...)
#endif

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
        dump_deltas("pop");
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
    if(result) dump_deltas("push (ins)");
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
        dump_deltas("push (rem)");
    }
}

typedef enum CursorMode {
    CursorMode_Normal,
    CursorMode_Second,
    CursorMode_Alpha,
} CursorMode;

typedef struct LoadedProgram {
    u8 data[32768];
    u16 size;
    
    s24 linebreaks[4096];
    s24 linebreaks_count;
    
    s24 cursor;
    
    bool cursor_selecting;
    s24 cursor_started_selecting;
    
    s24 view_top_line;
    
    u8 clipboard[256];
    u16 clipboard_size;
    
    DeltaCollection undo_buffer;
    DeltaCollection redo_buffer;
    
    // NOTE: Null if closed. If `opened_directory.name == "EXEC"`
    // then we have some hardcoded overrides to match TI-OS's functionality
    TokenDirectory *opened_directory;
    s24 opened_directory_list_index;
    s24 opened_directory_token_index;
    s24 opened_directory_view_top_token_index;
    
    CursorMode cursor_mode;
    bool alpha_is_lowercase;
} LoadedProgram;

static LoadedProgram program = {};
static bool global_running = true;

typedef struct OS_Program {
    u8 name[8];
} OS_Program;
OS_Program os_programs[512];
s24        os_programs_count;

u8 key_down[8];
u8 key_held[8];
u8 key_up[8];
u8 key_debounced[8];
u8 key_timers[8][8];

int main() {
    gfx_Begin();
    gfx_SetDrawBuffer();
    fontlib_SetFont(editor_font, 0);
    
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
                log("%s\n", name);
                for(char *val = name; val <= val + 7; val += 1, dest += 1) {
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
    // NOTE: Hard-coded functionality
    directories[DIR_PRGM].lists[3].tokens_count = cast(s16)os_programs_count;
    
    u8 load = ti_OpenVar("THEOMGTR", "r", OS_TYPE_PRGM);
    assert(load != 0, "Cannot open file");
    if(load != 0)
    {
        zero(&program, sizeof(LoadedProgram));
        program.linebreaks[0] = -1;
        program.linebreaks_count = 1;
        u16 size = ti_GetSize(load);
        assert((u24)size <= ARRLEN(program.data), "Program too big");
        if(size <= ARRLEN(program.data)) {
            u24 success = ti_Read(program.data, size, 1, load);
            assert(success, "Failed to read");
            if(success) {
                program.size = size;
                for(s24 i = 0; i <= program.size - 1;) {
                    if(program.data[i] == LINEBREAK) {
                        if(program.linebreaks_count == ARRLEN(program.linebreaks) - 1) {
                            // TODO: Graceful error handling
                            assert(cast(u24)program.linebreaks_count < ARRLEN(program.linebreaks) - 1, "Too big");
                        } else {
                            program.linebreaks_count += 1;
                            program.linebreaks[program.linebreaks_count - 1] = i;
                        }
                    }
                    i += get_token_size(i);
                }
            }
        }
    } else {
        zero(&program, sizeof(LoadedProgram));
        program.linebreaks[0] = -1;
        program.linebreaks_count = 1;
    }
    
    #define TARGET_FRAMERATE (15)
    #define TARGET_CLOCKS_PER_FRAME cast(s24)((cast(u24)CLOCKS_PER_SEC) / TARGET_FRAMERATE)
    
    s24 clock_counter = 0;
    u24 previous_clock = cast(u24)clock();
    
    global_running = true;
    while(global_running) {
        u24 current_clock = cast(u24)clock();
        s24 diff = cast(s24)(current_clock - previous_clock);
        previous_clock = current_clock;
        clock_counter -= diff;
        if(clock_counter <= 0) {
            clock_counter = TARGET_CLOCKS_PER_FRAME;
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
    
    gfx_End();
    
    return 0;
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
}

void offset_linebreaks(s24 from_here, s24 offset_by) {
    for(int i = program.linebreaks_count - 1; i >= 0; --i) {
        if(program.linebreaks[i] >= from_here) { program.linebreaks[i] += offset_by; }
        else { break; }
    }
}

void make_room_for_tokens(s24 first_index, s24 count) {
    for(int i = program.size - 1; i >= first_index + count; --i) {
        program.data[i] = program.data[i - count];
    }
}

// NOTE: Returns index of first linebreak that can be added
s24 make_room_for_linebreaks(s24 first_linebreaks_program_data_index, s24 count) {
    s24 result = -2;
    
    assert(count >= 1, "No room needed for linebreaks");
    for(s24 i = program.linebreaks_count - 1; i >= 0; --i) {
        if(program.linebreaks[i] < first_linebreaks_program_data_index)
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

#ifdef DEBUG
void dump(char *dump_name) {
    log("\n%s:\n", dump_name);
    assert(program.linebreaks[0] == -1, "First linebreak should be -1");
    int current_linebreak = 1;
    for(int i = 0; i <= program.size - 1; ++i) {
        bool is_linebreak = false;
        if(current_linebreak <= program.linebreaks_count - 1) {
            if(program.linebreaks[current_linebreak] == i) {
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
        log("%d,", program.linebreaks[i]);
    }
    log("\n");
}
#endif

// TODO: Check for off-by-one mistakes? No issues yet...
// NOTE: push_delta may be null if you do not want to push to an undo/redo buffer
void remove_tokens_(s24 at, u16 bytes_count, DeltaCollection* push_delta) {

    if(push_delta) {
        push_remove_delta(push_delta, program.cursor, at, program.data + at, bytes_count);
    }

    s24 linebreaks_count = 0;
    for(s24 i = at; i <= at + (bytes_count - 1); ++i) {
        if(program.data[i] == LINEBREAK) {
            linebreaks_count += 1;
        }
    }
    if(linebreaks_count >= 1) {
        s24 first_linebreak = -1;
        for(s24 i = 0; i <= program.linebreaks_count - 1; ++i) {
            if(program.linebreaks[i] >= at) {
                first_linebreak = i;
                break;
            }
        }
        assert(first_linebreak != -1, "Weird");
        for(s24 i = first_linebreak; i <= program.linebreaks_count - linebreaks_count - 1; ++i) {
            program.linebreaks[i] = program.linebreaks[i + linebreaks_count];
        }
        program.linebreaks_count -= linebreaks_count;
    }
    for(s24 i = at; i <= program.size - (bytes_count - 1); ++i) {
        program.data[i] = program.data[i + bytes_count];
    }
    offset_linebreaks(at, -1 * cast(s24)bytes_count);
    program.size -= bytes_count;
}

void remove_tokens(s24 at, u16 bytes_count) {
    remove_tokens_(at, bytes_count, &program.undo_buffer);
    clear_delta_collection(&program.redo_buffer);
}

// NOTE: push_delta may be null if you do not want to push to an undo/redo buffer
void insert_tokens_(s24 at, u8 *tokens, u16 bytes_count, DeltaCollection* push_delta) {
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
            program.linebreaks[add_linebreak_at] = i;
            add_linebreak_at += 1;
        }
    }
    
    if(push_delta) {
        push_insert_delta(push_delta, program.cursor, at, bytes_count);
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

s24 calculate_cursor_y(void) {
    s24 cursor_y = program.linebreaks_count - 1;
    for(int i = 0; i <= program.linebreaks_count - 1; ++i) {
        if(program.cursor <= program.linebreaks[i]) {
            cursor_y = i - 1;
            break;
        }
    }
    return cursor_y;
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

void update(void) {
    // NOTE: This will be updated after operations that affect cursor position/line breaks
    // We can afford to not update it after operations that affect cursor position but not line breaks
    // if we do that operation repeatedly and want to not iterate over linebreaks in the program much.
    s24 cursor_y = calculate_cursor_y();
    
    
    if(key_down[1] & kb_2nd) {
        if(program.cursor_mode != CursorMode_Second) {
            program.cursor_mode = CursorMode_Second;
        } else {
            program.cursor_mode = CursorMode_Normal;
        }
    }
    if(key_down[2] & kb_Alpha) {
        if(program.cursor_mode == CursorMode_Normal) {
            program.cursor_mode = CursorMode_Alpha;
            program.alpha_is_lowercase = false;
        } else if(program.cursor_mode == CursorMode_Second) {
            program.cursor_mode = CursorMode_Alpha;
            program.alpha_is_lowercase = true;
        } else if(program.cursor_mode == CursorMode_Alpha) {
            program.cursor_mode = CursorMode_Normal;
        }
    }
    
    if(program.opened_directory) {
        if((key_debounced[7] & kb_Right)) {
            program.opened_directory_list_index += 1;
            TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
            if(list->tokens_count >= 1 && program.opened_directory_token_index >= list->tokens_count) {
                program.opened_directory_token_index = list->tokens_count - 1;
            }
            if(list->tokens_count == 0) program.opened_directory_token_index = 0;
        }
        if((key_debounced[7] & kb_Left)) {
            program.opened_directory_list_index -= 1;
            TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
            if(list->tokens_count >= 1 && program.opened_directory_token_index >= list->tokens_count) {
                program.opened_directory_token_index = list->tokens_count - 1;
            }
            if(list->tokens_count == 0) program.opened_directory_token_index = 0;
        }
        if(program.opened_directory_list_index < 0) program.opened_directory_list_index += program.opened_directory->list_count;
        if(program.opened_directory_list_index >= program.opened_directory->list_count) program.opened_directory_list_index -= program.opened_directory->list_count;
        TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
        if(program.cursor_mode == CursorMode_Alpha) {
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
        if(program.cursor_mode != CursorMode_Alpha) {
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
        if(key_down[6] & kb_Enter) select = true;
        if(select) {
            bool hardcode = get_is_prgm_exec_override();
            if(hardcode) {
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
            } else {
                u16 token = list->tokens[program.opened_directory_token_index];
                if((token >> 8) == 0) {
                    insert_token_u8(program.cursor, cast(u8)token);
                    program.cursor += 1;
                } else {
                    insert_token_u16(program.cursor, token);
                    program.cursor += 2;
                }
            }
            program.opened_directory = null;
        }
        if(key_down[6] & kb_Clear) program.opened_directory = null;
    } else {
        if(key_down[6] & kb_Clear) global_running = false;
        
        if(program.cursor_mode == CursorMode_Normal) {
            // TODO: what does "X,T,0,n" output?
            // TODO: stat,math,apps,prgm,vars,draw,distr,matrix,test,list,catalog
            if(key_down[4] & kb_Prgm) {
                program.opened_directory = &directories[DIR_PRGM];
                program.opened_directory_list_index = 0;
                program.opened_directory_token_index = 0;
                key_down[4] &= ~kb_Prgm;
            }
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
        }
        else if(program.cursor_mode == CursorMode_Alpha) {
            if(key_down[2] & kb_Math)  { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB0BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'A'); program.cursor += 1; } } // A
            if(key_down[3] & kb_Apps)  { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB1BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'B'); program.cursor += 1; } } // B
            if(key_down[4] & kb_Prgm)  { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB2BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'C'); program.cursor += 1; } } // C
            if(key_down[2] & kb_Recip) { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB3BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'D'); program.cursor += 1; } } // D
            if(key_down[3] & kb_Sin)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB4BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'E'); program.cursor += 1; } } // E
            if(key_down[4] & kb_Cos)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB5BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'F'); program.cursor += 1; } } // F
            if(key_down[5] & kb_Tan)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB6BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'G'); program.cursor += 1; } } // G
            if(key_down[6] & kb_Power) { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB7BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'H'); program.cursor += 1; } } // H
            if(key_down[2] & kb_Square){ if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB8BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'I'); program.cursor += 1; } } // I
            if(key_down[3] & kb_Comma) { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xB9BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'J'); program.cursor += 1; } } // J
            if(key_down[4] & kb_LParen){ if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBABB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'K'); program.cursor += 1; } } // K
            if(key_down[5] & kb_RParen){ if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBCBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'L'); program.cursor += 1; } } // L
            if(key_down[6] & kb_Div)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBDBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'M'); program.cursor += 1; } } // M
            if(key_down[2] & kb_Log)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBEBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'N'); program.cursor += 1; } } // N
            if(key_down[3] & kb_7)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xBFBB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'O'); program.cursor += 1; } } // O
            if(key_down[4] & kb_8)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC0BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'P'); program.cursor += 1; } } // P
            if(key_down[5] & kb_9)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC1BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Q'); program.cursor += 1; } } // Q
            if(key_down[6] & kb_Mul)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC2BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'R'); program.cursor += 1; } } // R
            if(key_down[2] & kb_Ln)    { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC3BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'S'); program.cursor += 1; } } // S
            if(key_down[3] & kb_4)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC4BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'T'); program.cursor += 1; } } // T
            if(key_down[4] & kb_5)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC5BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'U'); program.cursor += 1; } } // U
            if(key_down[5] & kb_6)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC6BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'V'); program.cursor += 1; } } // V
            if(key_down[6] & kb_Sub)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC7BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'W'); program.cursor += 1; } } // W
            if(key_down[2] & kb_Sto)   { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC8BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'X'); program.cursor += 1; } } // X
            if(key_down[3] & kb_1)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xC9BB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Y'); program.cursor += 1; } } // Y
            if(key_down[4] & kb_2)     { if(program.alpha_is_lowercase) { insert_token_u16(program.cursor, 0xCABB); program.cursor += 2; } else { insert_token_u8(program.cursor, 'Z'); program.cursor += 1; } } // Z
            if(key_down[5] & kb_3)     { insert_token_u8(program.cursor, 0x5B); program.cursor += 1; }
            if(key_down[6] & kb_Add)   { insert_token_u8(program.cursor, 0x2A); program.cursor += 1; }
            if(key_down[3] & kb_0)     { insert_token_u8(program.cursor, SPACE); program.cursor += 1; }
            if(key_down[4] & kb_DecPnt){ insert_token_u8(program.cursor, 0x3E); program.cursor += 1; }
            if(key_down[5] & kb_Chs)   { insert_token_u8(program.cursor, 0xAF); program.cursor += 1; }
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
                if(size <= 256) {
                    program.cursor_selecting = false;
                    program.clipboard_size = (u16)size;
                    copy(program.data + range.min, program.clipboard, size);
                }
            } else {
                // Paste
                insert_tokens(program.cursor, program.clipboard, program.clipboard_size);
            }
            cursor_y = calculate_cursor_y();
        }
        if(key_down[1] & kb_Zoom) {
            if(program.cursor_selecting) {
                // Cut
                Range range = get_selecting_range();
                s24 size = (range.max + 1) - range.min;
                assert(size >= 0, "Underflow error");
                if(size <= 256) {
                    program.cursor_selecting = false;
                    program.clipboard_size = (u16)size;
                    copy(program.data + range.min, program.clipboard, size);
                    remove_tokens(range.min, (u16)size);
                    program.cursor = range.min;
                }
            } else {
                // Paste
                insert_tokens(program.cursor, program.clipboard, program.clipboard_size);
                program.cursor += program.clipboard_size;
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
                remove_tokens(program.cursor, get_token_size(program.cursor));
            }
            cursor_y = calculate_cursor_y();
        }
        if(key_down[1] & kb_Mode) {
            // NOTE: Clear line
            s24 line_start = program.linebreaks[cursor_y] + 1;
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
                if(size >= 0 && size < 65535) {
                    remove_tokens(line_start, cast(u16)size);
                    program.cursor = line_start;
                    cursor_y = calculate_cursor_y();
                } else {
                    assert(size >= 0 && size < 65535, "Must be within u16 range");
                }
            }
        }
        if(key_debounced[6] & kb_Enter) {
            insert_token_u8(program.cursor, LINEBREAK);
            program.cursor += 1;
        }
        if(key_debounced[7] & kb_Down) {
            s24 target_cursor = cursor_y;
            if(program.cursor_mode == CursorMode_Alpha) target_cursor += 8;
            else target_cursor += 1;
            if(target_cursor >= program.linebreaks_count - 1) target_cursor = program.linebreaks_count - 1;
            program.cursor = program.linebreaks[target_cursor] + 1;
        }
        if(key_debounced[7] & kb_Up) {
            s24 target_cursor = cursor_y;
            if(program.cursor_mode == CursorMode_Alpha) target_cursor -= 8;
            else target_cursor -= 1;
            if(target_cursor < 0) target_cursor = 0;
            program.cursor = program.linebreaks[target_cursor] + 1;
        }
        if(key_debounced[7] & kb_Left) {
            if(program.cursor > 0) {
                u8 prev_token_size = get_token_size(program.cursor - 1);
                program.cursor -= prev_token_size;
            }
        }
        if(key_debounced[7] & kb_Right) {
            u8 size = get_token_size(program.cursor);
            program.cursor += size;
            if(program.cursor >= program.size) {
                program.cursor = program.size;
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

void render(void) {
    gfx_FillScreen(BACKGROUND);
    if(program.opened_directory) {
        if(program.opened_directory_token_index - program.opened_directory_view_top_token_index >= 22) {
            program.opened_directory_view_top_token_index = program.opened_directory_token_index - 22;
        }
        if(program.opened_directory_view_top_token_index > program.opened_directory_token_index) {
            program.opened_directory_view_top_token_index = program.opened_directory_token_index;
        }
        u24 x = 5;
        for(int i = 0; i < program.opened_directory->list_count; ++i) {
            bool selected = (i == program.opened_directory_list_index);
            u24 width = (FONT_WIDTH*cast(u24)program.opened_directory->lists[i].name_count);
            if(!selected) {
                fontlib_SetForegroundColor(FOREGROUND);
                fontlib_SetBackgroundColor(BACKGROUND);
            } else {
                fontlib_SetForegroundColor(BACKGROUND);
                fontlib_SetBackgroundColor(FOREGROUND);
                gfx_SetColor(FOREGROUND);
                gfx_FillRectangle_NoClip(x - 2, 4, width + 4, FONT_HEIGHT + 2);
            }
            draw_string(program.opened_directory->lists[i].name, cast(u24)x, 5);
            x += width + 5;
        }
        TokenList *list = program.opened_directory->lists + program.opened_directory_list_index;
        u8 y = 15;
        bool is_prgm_exec_override = get_is_prgm_exec_override();
        for(u24 i = cast(u24)program.opened_directory_view_top_token_index; i < cast(u24)list->tokens_count && i < cast(u24)program.opened_directory_view_top_token_index + 22; ++i) {
            u24 str_length = 0;
            char *str;
            if(is_prgm_exec_override) {
                // NOTE: EXEC hard-coded override
                str = (char*)os_programs[i].name;
                str_length = 8;
                for(int i = 0; i <= 7; ++i) {
                    if(str[i] == 0) {
                        str_length = (u24)i;
                        break;
                    }
                }
            } else {
                u16 *ptr = list->tokens + i;
                str = ti_GetTokenString(cast(void**)&ptr, null, &str_length);
            }
            
            bool selected = (cast(s24)i == program.opened_directory_token_index);
            u24 width = (str_length+2)*FONT_WIDTH;
            if(!selected) {
                fontlib_SetForegroundColor(FOREGROUND);
                fontlib_SetBackgroundColor(BACKGROUND);
            } else {
                fontlib_SetForegroundColor(BACKGROUND);
                fontlib_SetBackgroundColor(FOREGROUND);
                gfx_SetColor(FOREGROUND);
                gfx_FillRectangle_NoClip(4, y - 1, width + 2, FONT_HEIGHT + 2);
            }
            
            char *key_table[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
            char *key = null;
            if(i >= 0 && i <= ARRLEN(key_table) - 1) { key = key_table[i]; }
            else { key = null; }
            if(key) { draw_string(key, 5, y); }
            if(is_prgm_exec_override) draw_string_max_chars(str, 8, 2*FONT_WIDTH+7, y);
            else draw_string(str, 2*FONT_WIDTH+7, y);
            y += FONT_HEIGHT + 2;
        }
        fontlib_SetForegroundColor(FOREGROUND);
        fontlib_SetBackgroundColor(BACKGROUND);
    } else {
        static u8 ascii_storage[2];
        ascii_storage[1] = 0;
        s24 cursor_y = program.linebreaks_count - 1;
        for(int i = 0; i <= program.linebreaks_count - 1; ++i) {
            if(program.cursor <= program.linebreaks[i]) {
                cursor_y = i - 1;
                break;
            }
        }
        
        fontlib_SetForegroundColor(FOREGROUND);
        fontlib_SetBackgroundColor(BACKGROUND);
        fontlib_SetFirstPrintableCodePoint(0);
        gfx_SetColor(FOREGROUND);
        int x = 5;
        u8 y = 5;
        if(cursor_y < program.view_top_line) {
            program.view_top_line = cursor_y;
        }
        if(cursor_y > program.view_top_line + 21) {
            program.view_top_line = cursor_y - 21;
        }
        int first_token = program.linebreaks[program.view_top_line] + 1;
        gfx_FillRectangle(0,y,2,FONT_HEIGHT);
        s24 current_view_y = program.view_top_line;
        Range selection;
        if(program.cursor_selecting) selection = get_selecting_range();
        for(int i = first_token; i < program.size;) {
            bool on_cursor = (i == program.cursor);
            bool selected = on_cursor;
            if(program.cursor_selecting) {
                selected = i >= selection.min && i <= selection.max;
            }
            if(selected) { fontlib_SetBackgroundColor(HIGHLIGHT); }
            else { fontlib_SetBackgroundColor(BACKGROUND); }
            u24 str_length = 0;
            char *str;
            u8 byte_0 = program.data[i];
            u8 token_size = get_token_size(i);
            
            if(byte_0 != LINEBREAK && byte_0 != SPACE) {
                u8 *ptr = program.data + i;
                str = ti_GetTokenString(cast(void**)&ptr, null, &str_length);
            }
            
            i += token_size;
            
            bool break_line = false;
            
            if(selected && str_length == 0 && (byte_0 == LINEBREAK || byte_0 == SPACE)) {
                s24 length_of_rect;
                if(byte_0 == LINEBREAK) { length_of_rect = FONT_WIDTH/2; }
                if(byte_0 == SPACE) { length_of_rect = FONT_WIDTH; }
                gfx_SetColor(HIGHLIGHT);
                gfx_FillRectangle(x,y,length_of_rect,FONT_HEIGHT);
                gfx_SetColor(FOREGROUND);
            }
            if(str_length > 0) {
                s24 start = x;
                s24 end = x + cast(s24)str_length*FONT_WIDTH;
                s24 max_chars = (min(310, end) - start) / FONT_WIDTH;
                if(end > 310) {
                    break_line = true;
                }
                if(max_chars > 0) {
                    draw_string_max_chars(str,(u24)max_chars, (u24)x,y);
                }
            }
            if(on_cursor && !program.cursor_selecting) {
                u8 length_of_rect = FONT_WIDTH;
                if(str_length > 0) { length_of_rect = cast(u8)str_length*FONT_WIDTH; }
                gfx_FillRectangle(x,y+FONT_HEIGHT+1,length_of_rect,3);
                gfx_FillRectangle(x-1,y,2,FONT_HEIGHT);
            }
            if(str_length > 0) {
                x += str_length*FONT_WIDTH;
            }
            if(byte_0 == SPACE) { x += FONT_WIDTH; }
            if(byte_0 == 0x3F) { break_line = true; }
            
            if(break_line) {
                x = 5;
                y += FONT_HEIGHT + 2;
                current_view_y += 1;
                if(current_view_y <= program.linebreaks_count - 1) {
                    i = program.linebreaks[current_view_y] + 1;
                    gfx_FillRectangle(0,y,2,FONT_HEIGHT);
                } else {
                    break;
                }
            }
            if(y+10 >= 230)
            {
                break;
            }
        }
        if(program.cursor >= program.size)
        {
            gfx_FillRectangle(x,y+FONT_HEIGHT+1,FONT_WIDTH,3);
            gfx_FillRectangle(x-1,y,2,FONT_HEIGHT);
        }
        
        u8 undo_bar_height = (u8)min(240, 3*program.undo_buffer.delta_count);
        gfx_FillRectangle(320-2,240-undo_bar_height,2,undo_bar_height);
        u8 redo_bar_height = (u8)min(240, 3*program.redo_buffer.delta_count);
        gfx_FillRectangle(320-4,240-redo_bar_height,2,redo_bar_height);
        
        //log("%2x %2x [%2x] %2x %2x\n", program.data[program.cursor - 2], program.data[program.cursor - 1], program.data[program.cursor], program.data[program.cursor + 1], program.data[program.cursor+2]);
    }
    
    fontlib_SetForegroundColor(FOREGROUND);
    fontlib_SetBackgroundColor(BACKGROUND);
    char *cursor_glyph = null;
    if(program.cursor_mode == CursorMode_Second) {
        cursor_glyph = "\xE1";
    } else if(program.cursor_mode == CursorMode_Alpha) {
        if(program.alpha_is_lowercase) {
            cursor_glyph = "\xE3";
        } else {
            cursor_glyph = "\xE2";
        }
    }
    if(cursor_glyph) {
        draw_string(cursor_glyph, 320-(FONT_WIDTH+2), 2);
    }
}

#if DEBUG
void dump_deltas(char *title)
{
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
#endif
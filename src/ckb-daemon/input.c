#include "input.h"
#include "notify.h"

int macromask(const uchar* key1, const uchar* key2){
    // Scan a macro against key input. Return 0 if any of them don't match
    for(int i = 0; i < N_KEYS / 8; i++){
        if((key1[i] & key2[i]) != key2[i])
            return 0;
    }
    return 1;
}

void inputupdate(usbdevice* kb){
#ifdef OS_LINUX
    if(!kb->uinput)
        return;
#else
    if(!kb->event)
        return;
#endif
    pthread_mutex_lock(&kb->mutex);
    usbmode* mode = kb->profile.currentmode;
    const key* keymap = kb->profile.keymap;
    keybind* bind = &mode->bind;
    // Don't do anything if the state hasn't changed
    if(!memcmp(kb->previntinput, kb->intinput, N_KEYS / 8)){
        pthread_mutex_unlock(&kb->mutex);
        return;
    }
    // Look for macros matching the current state
    int macrotrigger = 0;
    for(int i = 0; i < bind->macrocount; i++){
        keymacro* macro = &bind->macros[i];
        if(macromask(kb->intinput, macro->combo)){
            if(!macro->triggered){
                macrotrigger = 1;
                macro->triggered = 1;
                // Send events for each keypress in the macro
                for(int a = 0; a < macro->actioncount; a++)
                    os_keypress(kb, macro->actions[a].scan, macro->actions[a].down);
                os_kpsync(kb);
            }
        } else {
            macro->triggered = 0;
        }
    }
    for(int byte = 0; byte < N_KEYS / 8; byte++){
        char oldb = kb->previntinput[byte], newb = kb->intinput[byte];
        if(oldb == newb)
            continue;
        for(int bit = 0; bit < 8; bit++){
            int keyindex = byte * 8 + bit;
            const key* map = keymap + keyindex;
            int scancode = bind->base[keyindex];
            char mask = 1 << bit;
            char old = oldb & mask, new = newb & mask;
            // If the key state changed, send it to the uinput device
            if(old != new){
                // Don't echo a key press if a macro was triggered or if there's no scancode associated
                if(!macrotrigger && scancode >= 0){
                    os_keypress(kb, scancode, !!new);
                    // The volume wheel doesn't generate keyups, so create them automatically
                    if(new && (map->scan == KEY_VOLUMEUP || map->scan == KEY_VOLUMEDOWN)){
                        os_keypress(kb, scancode, 0);
                        kb->intinput[byte] &= ~mask;
                    }
                    os_kpsync(kb);
                }
                // Print a notification if desired
                int notify = mode->notify[byte] & mask;
                if(notify){
                    if(map->name){
                        nprintf(kb, 0, 0, "%s %s", map->name, new ? "down" : "up");
                        if(new && (map->scan == KEY_VOLUMEUP || map->scan == KEY_VOLUMEDOWN))
                            nprintf(kb, 0, 0, "%s up", map->name);
                    } else
                        nprintf(kb, 0, 0, "#%d %s", keyindex, new ? "down" : "up");
                }
            }
        }
    }
    memcpy(kb->previntinput, kb->intinput, N_KEYS / 8);
    pthread_mutex_unlock(&kb->mutex);
}

void initbind(keybind* bind, const key* keymap){
    for(int i = 0; i < N_KEYS; i++)
        bind->base[i] = keymap[i].scan;
    bind->macros = malloc(32 * sizeof(keymacro));
    bind->macrocap = 32;
    bind->macrocount = 0;
}

void closebind(keybind* bind){
    for(int i = 0; i < bind->macrocount; i++)
        free(bind->macros[i].actions);
    free(bind->macros);
    memset(bind, 0, sizeof(*bind));
}

void cmd_bind(usbmode* mode, const key* keymap, int keyindex, const char* to){
    // Find the key to bind to
    int tocode = 0;
    if(sscanf(to, "#x%ux", &tocode) != 1 && sscanf(to, "#%u", &tocode) == 1){
        mode->bind.base[keyindex] = tocode;
        return;
    }
    // If not numeric, look it up
    for(int i = 0; i < N_KEYS; i++){
        if(keymap[i].name && !strcmp(to, keymap[i].name)){
            mode->bind.base[keyindex] = keymap[i].scan;
            return;
        }
    }
}

void cmd_unbind(usbmode* mode, const key* keymap, int keyindex, const char* to){
    mode->bind.base[keyindex] = 0;
}

void cmd_rebind(usbmode* mode, const key* keymap, int keyindex, const char* to){
    mode->bind.base[keyindex] = keymap[keyindex].scan;
}

void cmd_macro(usbmode* mode, const key* keymap, const char* keys, const char* assignment){
    keybind* bind = &mode->bind;
    if(bind->macrocount >= MACRO_MAX)
        return;
    // Create a key macro
    keymacro macro;
    memset(&macro, 0, sizeof(macro));
    // Scan the left side for key names, separated by +
    int empty = 1;
    int left = strlen(keys), right = strlen(assignment);
    int position = 0, field = 0;
    char keyname[12];
    while(position < left && sscanf(keys + position, "%10[^+]%n", keyname, &field) == 1){
        int keycode;
        if((sscanf(keyname, "#%d", &keycode) && keycode >= 0 && keycode < N_KEYS)
                  || (sscanf(keyname, "#x%x", &keycode) && keycode >= 0 && keycode < N_KEYS)){
            // Set a key numerically
            SET_KEYBIT(macro.combo, keycode);
            empty = 0;
        } else {
            // Find this key in the keymap
            for(unsigned i = 0; i < N_KEYS; i++){
                if(keymap[i].name && !strcmp(keyname, keymap[i].name)){
                    macro.combo[i / 8] |= 1 << (i % 8);
                    empty = 0;
                    break;
                }
            }
        }
        if(keys[position += field] == '+')
            position++;
    }
    if(empty)
        return;
    // Count the number of actions (comma separated)
    int count = 0;
    for(const char* c = assignment; *c != 0; c++){
        if(*c == ',')
            count++;
    }
    // Allocate a buffer for them
    macro.actions = malloc(sizeof(macroaction) * count);
    macro.actioncount = 0;
    // Scan the actions
    position = 0;
    field = 0;
    while(position < right && sscanf(assignment + position, "%11[^,]%n", keyname, &field) == 1){
        if(!strcmp(keyname, "clear"))
            break;
        int down = (keyname[0] == '+');
        if(down || keyname[0] == '-'){
            int keycode;
            if((sscanf(keyname + 1, "#%d", &keycode) && keycode >= 0 && keycode < N_KEYS)
                      || (sscanf(keyname + 1, "#x%x", &keycode) && keycode >= 0 && keycode < N_KEYS)){
                // Set a key numerically
                macro.actions[macro.actioncount].scan = keymap[keycode].scan;
                macro.actions[macro.actioncount].down = down;
                macro.actioncount++;
            } else {
                // Find this key in the keymap
                for(unsigned i = 0; i < N_KEYS; i++){
                    if(keymap[i].name && !strcmp(keyname + 1, keymap[i].name)){
                        macro.actions[macro.actioncount].scan = keymap[i].scan;
                        macro.actions[macro.actioncount].down = down;
                        macro.actioncount++;
                        break;
                    }
                }
            }
        }
        if(assignment[position += field] == ',')
            position++;
    }

    // See if there's already a macro with this trigger
    keymacro* macros = bind->macros;
    for(int i = 0; i < bind->macrocount; i++){
        if(!memcmp(macros[i].combo, macro.combo, N_KEYS / 8)){
            free(macros[i].actions);
            // If the new macro has no actions, erase the existing one
            if(!macro.actioncount){
                for(int j = i + 1; j < bind->macrocount; j++)
                    memcpy(macros + j - 1, macros + j, sizeof(keymacro));
                bind->macrocount--;
            } else
                // If there are actions, replace the existing with the new
                memcpy(macros + i, &macro, sizeof(keymacro));
            return;
        }
    }

    // Add the macro to the device settings if not empty
    if(macro.actioncount < 1)
        return;
    memcpy(bind->macros + (bind->macrocount++), &macro, sizeof(keymacro));
    if(bind->macrocount >= bind->macrocap)
        bind->macros = realloc(bind->macros, (bind->macrocap += 16) * sizeof(keymacro));
}

void cmd_macroclear(usbmode* mode){
    keybind* bind = &mode->bind;
    for(int i = 0; i < bind->macrocount; i++)
        free(bind->macros[i].actions);
    bind->macrocount = 0;
}

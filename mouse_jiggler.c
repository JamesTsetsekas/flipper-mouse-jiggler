#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <mouse_jiggler_icons.h>

#define APP_VERSION "1.2"
#define ERROR_MESSAGE_ROTATE_MS 5000
#define MOTION_POLL_MS 25
#define MOTION_IDLE_MIN_MS 2500
#define MOTION_IDLE_MAX_MS 9000
#define MOTION_STEP_MIN_MS 70
#define MOTION_STEP_MAX_MS 260
#define MOTION_WANDER_MIN_STEPS 3
#define MOTION_WANDER_MAX_STEPS 8
#define MOTION_MAX_OFFSET 8
#define MOTION_MAX_STEP 2

typedef enum {
    UsbStateSwitching,
    UsbStateActive,
    UsbStateError,
} UsbState;

typedef enum {
    MotionPhaseIdle,
    MotionPhaseWander,
    MotionPhaseReturn,
} MotionPhase;

typedef struct {
    MotionPhase phase;
    int8_t offset_x;
    int8_t offset_y;
    uint8_t steps_remaining;
    uint32_t next_move_tick;
} MouseMotionState;

typedef struct {
    UsbState state;
    bool show_unplug_message;
    bool paused;
    uint32_t last_message_switch_tick;
} MouseJigglerContext;

static uint32_t mouse_jiggler_random_below(uint32_t upper_bound) {
    furi_assert(upper_bound > 0);

    // Rejection sampling avoids the small modulo bias produced by random % upper_bound.
    const uint32_t rejection_threshold = (uint32_t)(-upper_bound) % upper_bound;
    uint32_t value;

    do {
        value = furi_hal_random_get();
    } while(value < rejection_threshold);

    return value % upper_bound;
}

static uint32_t mouse_jiggler_random_range(uint32_t minimum, uint32_t maximum) {
    furi_assert(minimum <= maximum);
    return minimum + mouse_jiggler_random_below(maximum - minimum + 1);
}

static int8_t mouse_jiggler_random_step(void) {
    return (int8_t)mouse_jiggler_random_range(0, MOTION_MAX_STEP * 2) - MOTION_MAX_STEP;
}

static int8_t mouse_jiggler_clamp_step(int16_t value) {
    if(value > MOTION_MAX_STEP) {
        return MOTION_MAX_STEP;
    }
    if(value < -MOTION_MAX_STEP) {
        return -MOTION_MAX_STEP;
    }
    return (int8_t)value;
}

static bool mouse_jiggler_deadline_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void mouse_jiggler_schedule(
    MouseMotionState* motion,
    uint32_t now,
    uint32_t minimum_ms,
    uint32_t maximum_ms) {
    motion->next_move_tick =
        now + furi_ms_to_ticks(mouse_jiggler_random_range(minimum_ms, maximum_ms));
}

static void mouse_jiggler_motion_reset(MouseMotionState* motion) {
    motion->phase = MotionPhaseIdle;
    motion->offset_x = 0;
    motion->offset_y = 0;
    motion->steps_remaining = 0;
    mouse_jiggler_schedule(
        motion, furi_get_tick(), MOTION_IDLE_MIN_MS, MOTION_IDLE_MAX_MS);
}

static void mouse_jiggler_return_to_origin(MouseMotionState* motion) {
    if(motion->offset_x != 0 || motion->offset_y != 0) {
        furi_hal_hid_mouse_move(-motion->offset_x, -motion->offset_y);
    }

    motion->offset_x = 0;
    motion->offset_y = 0;
    motion->phase = MotionPhaseIdle;
    motion->steps_remaining = 0;
}

static void mouse_jiggler_render_callback(Canvas* canvas, void* ctx) {
    MouseJigglerContext* app_ctx = ctx;
    canvas_clear(canvas);

    canvas_draw_icon(canvas, 0, 0, &I_mouse_jiggler);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 14, 9, "Mouse Jiggler v");
    canvas_draw_str(canvas, 94, 9, APP_VERSION);

    canvas_set_font(canvas, FontSecondary);

    switch(app_ctx->state) {
        case UsbStateSwitching:
            canvas_draw_str(canvas, 0, 33, "Switching to HID mode...");
            canvas_draw_str(canvas, 0, 43, "Please wait");
            break;
        case UsbStateError:
            if(app_ctx->show_unplug_message) {
                canvas_draw_str(canvas, 0, 33, "Try unplugging your Flipper");
                canvas_draw_str(canvas, 0, 43, "and plug it in again");
            } else {
                canvas_draw_str(canvas, 0, 33, "Switch to HID mode failed");
                canvas_draw_str(canvas, 0, 43, "Retrying...");
            }
            break;
        case UsbStateActive:
        default:
            if(app_ctx->paused) {
                canvas_draw_str(canvas, 0, 33, "Motion paused");
                canvas_draw_str(canvas, 0, 43, "Press [OK] to resume");
            } else {
                canvas_draw_str(canvas, 0, 33, "Random motion active");
                canvas_draw_str(canvas, 0, 43, "Press [OK] to pause");
            }
    }

    canvas_draw_str(canvas, 0, 63, "Hold [back] to exit");
}

static void mouse_jiggler_input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

static void mouse_jiggler_jiggle(void* ctx) {
    MouseMotionState* motion = ctx;
    const uint32_t now = furi_get_tick();

    if(!mouse_jiggler_deadline_reached(now, motion->next_move_tick)) {
        return;
    }

    if(motion->phase == MotionPhaseIdle) {
        motion->phase = MotionPhaseWander;
        motion->steps_remaining = mouse_jiggler_random_range(
            MOTION_WANDER_MIN_STEPS, MOTION_WANDER_MAX_STEPS);
    }

    if(motion->phase == MotionPhaseWander) {
        int8_t dx;
        int8_t dy;

        do {
            dx = mouse_jiggler_random_step();
            dy = mouse_jiggler_random_step();

            if(motion->offset_x + dx > MOTION_MAX_OFFSET) {
                dx = MOTION_MAX_OFFSET - motion->offset_x;
            } else if(motion->offset_x + dx < -MOTION_MAX_OFFSET) {
                dx = -MOTION_MAX_OFFSET - motion->offset_x;
            }

            if(motion->offset_y + dy > MOTION_MAX_OFFSET) {
                dy = MOTION_MAX_OFFSET - motion->offset_y;
            } else if(motion->offset_y + dy < -MOTION_MAX_OFFSET) {
                dy = -MOTION_MAX_OFFSET - motion->offset_y;
            }
        } while(dx == 0 && dy == 0);

        furi_hal_hid_mouse_move(dx, dy);
        motion->offset_x += dx;
        motion->offset_y += dy;
        motion->steps_remaining--;

        if(motion->steps_remaining == 0) {
            motion->phase = MotionPhaseReturn;
        }

        mouse_jiggler_schedule(motion, now, MOTION_STEP_MIN_MS, MOTION_STEP_MAX_MS);
        return;
    }

    if(motion->offset_x == 0 && motion->offset_y == 0) {
        motion->phase = MotionPhaseIdle;
        mouse_jiggler_schedule(motion, now, MOTION_IDLE_MIN_MS, MOTION_IDLE_MAX_MS);
        return;
    }

    const int8_t dx = mouse_jiggler_clamp_step(-motion->offset_x);
    const int8_t dy = mouse_jiggler_clamp_step(-motion->offset_y);
    furi_hal_hid_mouse_move(dx, dy);
    motion->offset_x += dx;
    motion->offset_y += dy;

    if(motion->offset_x == 0 && motion->offset_y == 0) {
        motion->phase = MotionPhaseIdle;
        mouse_jiggler_schedule(motion, now, MOTION_IDLE_MIN_MS, MOTION_IDLE_MAX_MS);
    } else {
        mouse_jiggler_schedule(motion, now, MOTION_STEP_MIN_MS, MOTION_STEP_MAX_MS);
    }
}

int32_t mouse_jiggler_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(event_queue);

    ViewPort* view_port = view_port_alloc();

    FuriHalUsbInterface* usb_mode_prev = furi_hal_usb_get_config();
    MouseMotionState motion_state;
    MouseJigglerContext app_ctx = {
        .state = UsbStateSwitching,
        .show_unplug_message = false,
        .paused = false,
        .last_message_switch_tick = 0,
    };
    FuriTimer* timer =
        furi_timer_alloc(mouse_jiggler_jiggle, FuriTimerTypePeriodic, &motion_state);

    view_port_draw_callback_set(view_port, mouse_jiggler_render_callback, &app_ctx);
    view_port_input_callback_set(view_port, mouse_jiggler_input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool usb_switch_success = false;

    InputEvent event;

    while(1) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, usb_switch_success ? FuriWaitForever : 100);
        
        if(event_status == FuriStatusOk && event.type == InputTypeLong && event.key == InputKeyBack) {
            break;
        }

        if(event_status == FuriStatusOk && event.type == InputTypeShort &&
           event.key == InputKeyOk && usb_switch_success) {
            app_ctx.paused = !app_ctx.paused;

            if(app_ctx.paused) {
                furi_timer_stop(timer);
                mouse_jiggler_return_to_origin(&motion_state);
            } else {
                mouse_jiggler_motion_reset(&motion_state);
                furi_timer_start(timer, furi_ms_to_ticks(MOTION_POLL_MS));
            }
        }

        if(!usb_switch_success) {
            if(furi_hal_usb_set_config(&usb_hid, NULL)) {
                usb_switch_success = true;
                app_ctx.state = UsbStateActive;
                mouse_jiggler_motion_reset(&motion_state);
                furi_timer_start(timer, furi_ms_to_ticks(MOTION_POLL_MS));
            } else {
                if(app_ctx.state != UsbStateError) {
                    app_ctx.last_message_switch_tick = furi_get_tick();
                }
                app_ctx.state = UsbStateError;
                furi_delay_ms(200);
            }
        }

        if(app_ctx.state == UsbStateError) {
            uint32_t now = furi_get_tick();
            if((now - app_ctx.last_message_switch_tick) >= ERROR_MESSAGE_ROTATE_MS) {
                app_ctx.show_unplug_message = !app_ctx.show_unplug_message;
                app_ctx.last_message_switch_tick = now;
            }
        }

        view_port_update(view_port);
    }

    if(usb_switch_success && !app_ctx.paused) {
        furi_timer_stop(timer);
        mouse_jiggler_return_to_origin(&motion_state);
    }
    furi_hal_usb_set_config(usb_mode_prev, NULL);

    // remove & free all stuff created by app
    furi_timer_free(timer);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);

    return 0;
}

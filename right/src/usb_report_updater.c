#include <math.h>
#include "key_action.h"
#include "led_display.h"
#include "layer.h"
#include "usb_interfaces/usb_interface_mouse.h"
#include "keymap.h"
#include "peripherals/test_led.h"
#include "slave_drivers/is31fl3731_driver.h"
#include "slave_drivers/uhk_module_driver.h"
#include "macros.h"
#include "right_key_matrix.h"
#include "layer.h"
#include "usb_report_updater.h"
#include "timer.h"
#include "config_parser/parse_keymap.h"
#include "usb_commands/usb_command_get_debug_buffer.h"
#include "arduino_hid/ConsumerAPI.h"
#include "macro_recorder.h"
#include "macro_shortcut_parser.h"
#include "postponer.h"

static uint32_t mouseUsbReportUpdateTime = 0;
static uint32_t mouseElapsedTime;

uint16_t DoubleTapSwitchLayerTimeout = 300;
static uint16_t DoubleTapSwitchLayerReleaseTimeout = 200;

static bool toggledMouseStates[ACTIVE_MOUSE_STATES_COUNT];
static bool activeMouseStates[ACTIVE_MOUSE_STATES_COUNT];
bool TestUsbStack = false;
static key_action_t actionCache[SLOT_COUNT][MAX_KEY_COUNT_PER_MODULE];

volatile uint8_t UsbReportUpdateSemaphore = 0;

uint8_t HardwareModifierState;
uint8_t HardwareModifierStatePrevious;
bool SuppressMods = false;
bool SuppressKeys = false;
bool SuppressingKeys = false;
bool StickyModifiersEnabled = true;
bool SplitCompositeKeystroke = true;
uint16_t KeystrokeDelay = 0;
uint32_t KeystrokeDelayStarted = 0;
bool ActivateOnRelease = false;

key_state_t* EmergencyKey = NULL;

mouse_kinetic_state_t MouseMoveState = {
    .isScroll = false,
    .upState = SerializedMouseAction_MoveUp,
    .downState = SerializedMouseAction_MoveDown,
    .leftState = SerializedMouseAction_MoveLeft,
    .rightState = SerializedMouseAction_MoveRight,
    .intMultiplier = 25,
    .initialSpeed = 5,
    .acceleration = 35,
    .deceleratedSpeed = 10,
    .baseSpeed = 40,
    .acceleratedSpeed = 80,
};

mouse_kinetic_state_t MouseScrollState = {
    .isScroll = true,
    .upState = SerializedMouseAction_ScrollDown,
    .downState = SerializedMouseAction_ScrollUp,
    .leftState = SerializedMouseAction_ScrollLeft,
    .rightState = SerializedMouseAction_ScrollRight,
    .intMultiplier = 1,
    .initialSpeed = 20,
    .acceleration = 20,
    .deceleratedSpeed = 10,
    .baseSpeed = 20,
    .acceleratedSpeed = 50,
};

void ToggleMouseState(serialized_mouse_action_t action, bool activate)
{
    toggledMouseStates[action] = activate;
}

static void processMouseKineticState(mouse_kinetic_state_t *kineticState)
{
    float initialSpeed = kineticState->intMultiplier * kineticState->initialSpeed;
    float acceleration = kineticState->intMultiplier * kineticState->acceleration;
    float deceleratedSpeed = kineticState->intMultiplier * kineticState->deceleratedSpeed;
    float baseSpeed = kineticState->intMultiplier * kineticState->baseSpeed;
    float acceleratedSpeed = kineticState->intMultiplier * kineticState->acceleratedSpeed;

    if (!kineticState->wasMoveAction && !activeMouseStates[SerializedMouseAction_Decelerate]) {
        kineticState->currentSpeed = initialSpeed;
    }

    bool isMoveAction = activeMouseStates[kineticState->upState] ||
                        activeMouseStates[kineticState->downState] ||
                        activeMouseStates[kineticState->leftState] ||
                        activeMouseStates[kineticState->rightState];

    mouse_speed_t mouseSpeed = MouseSpeed_Normal;
    if (activeMouseStates[SerializedMouseAction_Accelerate]) {
        kineticState->targetSpeed = acceleratedSpeed;
        mouseSpeed = MouseSpeed_Accelerated;
    } else if (activeMouseStates[SerializedMouseAction_Decelerate]) {
        kineticState->targetSpeed = deceleratedSpeed;
        mouseSpeed = MouseSpeed_Decelerated;
    } else if (isMoveAction) {
        kineticState->targetSpeed = baseSpeed;
    }

    if (mouseSpeed == MouseSpeed_Accelerated || (kineticState->wasMoveAction && isMoveAction && (kineticState->prevMouseSpeed != mouseSpeed))) {
        kineticState->currentSpeed = kineticState->targetSpeed;
    }

    if (isMoveAction) {
        if (kineticState->currentSpeed < kineticState->targetSpeed) {
            kineticState->currentSpeed += acceleration * (float)mouseElapsedTime / 1000.0f;
            if (kineticState->currentSpeed > kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        } else {
            kineticState->currentSpeed -= acceleration * (float)mouseElapsedTime / 1000.0f;
            if (kineticState->currentSpeed < kineticState->targetSpeed) {
                kineticState->currentSpeed = kineticState->targetSpeed;
            }
        }

        float distance = kineticState->currentSpeed * (float)mouseElapsedTime / 1000.0f;


        if (kineticState->isScroll && !kineticState->wasMoveAction) {
            kineticState->xSum = 0;
            kineticState->ySum = 0;
        }

        // Update horizontal state

        bool horizontalMovement = true;
        if (activeMouseStates[kineticState->leftState]) {
            kineticState->xSum -= distance;
        } else if (activeMouseStates[kineticState->rightState]) {
            kineticState->xSum += distance;
        } else {
            horizontalMovement = false;
        }

        float xSumInt;
        float xSumFrac = modff(kineticState->xSum, &xSumInt);
        kineticState->xSum = xSumFrac;
        kineticState->xOut = xSumInt;

        if (kineticState->isScroll && !kineticState->wasMoveAction && kineticState->xOut == 0 && horizontalMovement) {
            kineticState->xOut = kineticState->xSum ? copysignf(1.0, kineticState->xSum) : 0;
            kineticState->xSum = 0;
        }

        // Update vertical state

        bool verticalMovement = true;
        if (activeMouseStates[kineticState->upState]) {
            kineticState->ySum -= distance;
        } else if (activeMouseStates[kineticState->downState]) {
            kineticState->ySum += distance;
        } else {
            verticalMovement = false;
        }

        float ySumInt;
        float ySumFrac = modff(kineticState->ySum, &ySumInt);
        kineticState->ySum = ySumFrac;
        kineticState->yOut = ySumInt;

        if (kineticState->isScroll && !kineticState->wasMoveAction && kineticState->yOut == 0 && verticalMovement) {
            kineticState->yOut = kineticState->ySum ? copysignf(1.0, kineticState->ySum) : 0;
            kineticState->ySum = 0;
        }
    } else {
        kineticState->currentSpeed = 0;
    }

    kineticState->prevMouseSpeed = mouseSpeed;
    kineticState->wasMoveAction = isMoveAction;
}

static void processMouseActions()
{
    mouseElapsedTime = Timer_GetElapsedTimeAndSetCurrent(&mouseUsbReportUpdateTime);

    processMouseKineticState(&MouseMoveState);
    ActiveUsbMouseReport->x += MouseMoveState.xOut;
    ActiveUsbMouseReport->y += MouseMoveState.yOut;
    MouseMoveState.xOut = 0;
    MouseMoveState.yOut = 0;

    processMouseKineticState(&MouseScrollState);
    ActiveUsbMouseReport->wheelX += MouseScrollState.xOut;
    ActiveUsbMouseReport->wheelY += MouseScrollState.yOut;
    MouseScrollState.xOut = 0;
    MouseScrollState.yOut = 0;

    for (uint8_t moduleId=0; moduleId<UHK_MODULE_MAX_COUNT; moduleId++) {
        uhk_module_state_t *moduleState = UhkModuleStates + moduleId;
        if (moduleState->pointerCount) {
            ActiveUsbMouseReport->wheelX += moduleState->pointerDelta.x;
            ActiveUsbMouseReport->wheelY -= moduleState->pointerDelta.y;
            moduleState->pointerDelta.x = 0;
            moduleState->pointerDelta.y = 0;
        }
    }

//  The following line makes the firmware crash for some reason:
//  SetDebugBufferFloat(60, mouseScrollState.currentSpeed);
//  TODO: Figure out why.
//  Oddly, the following line (which is the inlined version of the above) works:
//  *(float*)(DebugBuffer + 60) = mouseScrollState.currentSpeed;
//  The value parameter of SetDebugBufferFloat() seems to be the culprit because
//  if it's not used within the function it doesn't crash anymore.

    if (activeMouseStates[SerializedMouseAction_LeftClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Left;
    }
    if (activeMouseStates[SerializedMouseAction_MiddleClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Middle;
    }
    if (activeMouseStates[SerializedMouseAction_RightClick]) {
        ActiveUsbMouseReport->buttons |= MouseButton_Right;
    }
}

static layer_id_t previousLayer = LayerId_Base;

static void handleSwitchLayerAction(key_state_t *keyState, key_action_t *action)
{
    static key_state_t *doubleTapSwitchLayerKey;
    static uint32_t doubleTapSwitchLayerStartTime;
    static uint32_t doubleTapSwitchLayerTriggerTime;
    static bool isLayerDoubleTapToggled;

    if (doubleTapSwitchLayerKey && doubleTapSwitchLayerKey != keyState && ACTIVATED_NOW(keyState)) {
        doubleTapSwitchLayerKey = NULL;
    }

    if (action->type != KeyActionType_SwitchLayer) {
        return;
    }

    if (ACTIVATED_NOW(keyState) && isLayerDoubleTapToggled && ToggledLayer == action->switchLayer.layer) {
        ToggledLayer = LayerId_Base;
        isLayerDoubleTapToggled = false;
    }

    if (ACTIVE(keyState) && doubleTapSwitchLayerKey == keyState &&
        Timer_GetElapsedTime(&doubleTapSwitchLayerTriggerTime) > DoubleTapSwitchLayerReleaseTimeout)
    {
        ToggledLayer = LayerId_Base;
    }

    if (ACTIVATED_NOW(keyState) && previousLayer == LayerId_Base && action->switchLayer.mode == SwitchLayerMode_HoldAndDoubleTapToggle) {
        if (doubleTapSwitchLayerKey && Timer_GetElapsedTimeAndSetCurrent(&doubleTapSwitchLayerStartTime) < DoubleTapSwitchLayerTimeout) {
            ToggledLayer = action->switchLayer.layer;
            isLayerDoubleTapToggled = true;
            doubleTapSwitchLayerTriggerTime = CurrentTime;
        } else {
            doubleTapSwitchLayerKey = keyState;
        }
        doubleTapSwitchLayerStartTime = CurrentTime;
    }
}

static uint8_t basicScancodeIndex = 0;
static uint8_t mediaScancodeIndex = 0;
static uint8_t systemScancodeIndex = 0;
static uint8_t stickyModifiers, stickySlotId, stickyKeyId;
static uint8_t secondaryRoleState = SecondaryRoleState_Released;
static uint8_t secondaryRoleSlotId;
static uint8_t secondaryRoleKeyId;
static secondary_role_t secondaryRole;

static void applyKeyAction(key_state_t *keyState, key_action_t *action, uint8_t slotId, uint8_t keyId)
{
  bool isKeystrokeActivatedOnRelease = ActivateOnRelease && action->type == KeyActionType_Keystroke && action->keystroke.modifiers == 0 && action->keystroke.secondaryRole == 0;
  if (ACTIVE(keyState) || isKeystrokeActivatedOnRelease) {

    handleSwitchLayerAction(keyState, action);

    if (ACTIVATED_NOW(keyState)) {
        Macros_SignalInterrupt();
    }

    switch (action->type) {
        case KeyActionType_Keystroke:
            if (action->keystroke.scancode) {
                if (ACTIVATED_NOW(keyState)) {
                    stickyModifiers = action->keystroke.modifiers;
                    stickySlotId = slotId;
                    stickyKeyId = keyId;
#ifdef DEBUG_POSTPONER
                    char str[1];
                    str[0] = MacroShortcutParser_ScancodeToCharacter(action->keystroke.scancode);
                    Macros_SetStatusString("> ", NULL);
                    Macros_SetStatusChar(MacroShortcutParser_ScancodeToCharacter(action->keystroke.scancode));
                    Macros_SetStatusString("\n", NULL);
#endif
                }
            }
            if(!SuppressMods) {
                ActiveUsbBasicKeyboardReport->modifiers |= action->keystroke.modifiers;
            }
            HardwareModifierState |= action->keystroke.modifiers;
            if (action->keystroke.modifiers == 0 || SplitCompositeKeystroke == 0 || ACTIVATED_EARLIER(keyState)) {
                switch (action->keystroke.keystrokeType) {
                    case KeystrokeType_Basic:
                        if (basicScancodeIndex >= USB_BASIC_KEYBOARD_MAX_KEYS || action->keystroke.scancode == 0) {
                            break;
                        }
                        ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = action->keystroke.scancode;
                        break;
                    case KeystrokeType_Media:
                        if (mediaScancodeIndex >= USB_MEDIA_KEYBOARD_MAX_KEYS) {
                            break;
                        }
                        ActiveUsbMediaKeyboardReport->scancodes[mediaScancodeIndex++] = action->keystroke.scancode;
                        break;
                    case KeystrokeType_System:
                        if (systemScancodeIndex >= USB_SYSTEM_KEYBOARD_MAX_KEYS) {
                            break;
                        }
                        ActiveUsbSystemKeyboardReport->scancodes[systemScancodeIndex++] = action->keystroke.scancode;
                        break;
                }
            }
            break;
        case KeyActionType_Mouse:
            if (ACTIVATED_NOW(keyState)) {
                stickyModifiers = 0;
            }
            activeMouseStates[action->mouseAction] = true;
            break;
        case KeyActionType_SwitchLayer:
            // Handled by handleSwitchLayerAction()
            break;
        case KeyActionType_SwitchKeymap:
            if (ACTIVATED_NOW(keyState)) {
                stickyModifiers = 0;
                secondaryRoleState = SecondaryRoleState_Released;
                SwitchKeymapById(action->switchKeymap.keymapId);
                Macros_UpdateLayerStack();
            }
            break;
        case KeyActionType_PlayMacro:
            if (ACTIVATED_NOW(keyState)) {
                stickyModifiers = 0;
                Macros_StartMacro(action->playMacro.macroId, keyState, 255);
            }
            break;
        }
    } else {
        switch (action->type) {
            case KeyActionType_Keystroke:
                if (DEACTIVATED_EARLIER(keyState)) {
                    if (slotId == stickySlotId && keyId == stickyKeyId) {
                        if (!IsLayerHeld() && !(secondaryRoleState == SecondaryRoleState_Triggered && IS_SECONDARY_ROLE_LAYER_SWITCHER(secondaryRole))) {
                            stickyModifiers = 0;
                        }
                    }
                }
                break;
        }
    }
}

void clearActiveReports(void)
{
    memset(ActiveUsbMouseReport, 0, sizeof *ActiveUsbMouseReport);
    memset(ActiveUsbBasicKeyboardReport, 0, sizeof *ActiveUsbBasicKeyboardReport);
    memset(ActiveUsbMediaKeyboardReport, 0, sizeof *ActiveUsbMediaKeyboardReport);
    memset(ActiveUsbSystemKeyboardReport, 0, sizeof *ActiveUsbSystemKeyboardReport);
    basicScancodeIndex = 0;
    mediaScancodeIndex = 0;
    systemScancodeIndex = 0;
}

void mergeReports(void)
{
    for(uint8_t j = 0; j < MACRO_STATE_POOL_SIZE; j++) {
        if(MacroState[j].reportsUsed) {
            //if the macro ended right now, we still want to flush the last report
            MacroState[j].reportsUsed &= MacroState[j].macroPlaying;
            macro_state_t *s = &MacroState[j];
            ActiveUsbBasicKeyboardReport->modifiers |= s->macroBasicKeyboardReport.modifiers;
            for ( int i = 0; i < USB_BASIC_KEYBOARD_MAX_KEYS && s->macroBasicKeyboardReport.scancodes[i] != 0; i++) {
                if( basicScancodeIndex < USB_BASIC_KEYBOARD_MAX_KEYS ) {
                    ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = s->macroBasicKeyboardReport.scancodes[i];
                }
            }
            for ( int i = 0; i < USB_MEDIA_KEYBOARD_MAX_KEYS && s->macroMediaKeyboardReport.scancodes[i] != 0 ; i++) {
                if( mediaScancodeIndex < USB_MEDIA_KEYBOARD_MAX_KEYS ) {
                    ActiveUsbMediaKeyboardReport->scancodes[mediaScancodeIndex++] = s->macroMediaKeyboardReport.scancodes[i];
                }
            }
            for ( int i = 0; i < USB_SYSTEM_KEYBOARD_MAX_KEYS && s->macroSystemKeyboardReport.scancodes[i] != 0; i++) {
                if( systemScancodeIndex < USB_SYSTEM_KEYBOARD_MAX_KEYS ) {
                    ActiveUsbSystemKeyboardReport->scancodes[systemScancodeIndex++] = s->macroSystemKeyboardReport.scancodes[i];
                }
            }
            ActiveUsbMouseReport->buttons |= s->macroMouseReport.buttons;
            ActiveUsbMouseReport->x += s->macroMouseReport.x;
            ActiveUsbMouseReport->y += s->macroMouseReport.y;
            ActiveUsbMouseReport->wheelX += s->macroMouseReport.wheelX;
            ActiveUsbMouseReport->wheelY += s->macroMouseReport.wheelY;
        }
    }
}

void ActivateKey(key_state_t *keyState, bool debounce) {
    keyState->previous = 0;
    keyState->current = KeyState_Hw | KeyState_HwDebounced | KeyState_Sw;
    if(debounce) {
        keyState->timestamp = CurrentTime;
        keyState->debouncing = true;
    }
}

static inline void preprocessKeyState(key_state_t *keyState) {
    if(keyState->current == 0 && !keyState->debouncing && keyState != Postponer_NextEventKey) {
        return;
    }

    keyState->previous = keyState->current;
    bool prevDB = keyState->previous & KeyState_HwDebounced;
    bool prevSW = keyState->previous & KeyState_Sw;
    bool currHW = keyState->current & KeyState_Hw;
    bool currDB = currHW;

    if (keyState->debouncing) {
        if ((uint8_t)(CurrentTime - keyState->timestamp) > (prevDB ? DebounceTimePress : DebounceTimeRelease)) {
            keyState->debouncing = false;
        }
        currDB = prevDB;
    } else if (prevDB != currHW) {
        keyState->timestamp = CurrentTime;
        keyState->debouncing = true;
    }

    bool currSW = currDB && !keyState->suppressed;

     if (PostponerCore_IsActive() && keyState != EmergencyKey) {
        currSW = prevSW;
        if(currDB != prevDB) {
            PostponerCore_TrackKey(keyState, currDB);
        }
        if(Postponer_NextEventKey == keyState /* TODO: && !PostponeKeys*/) {
            currSW = PostponerCore_RunKey(keyState, currSW);
        }
    } else if (SuppressKeys) {
        currSW = currDB && prevSW && !keyState->suppressed;
        keyState->suppressed = !currSW;
    }

    keyState->suppressed &= currDB;
    keyState->current = KEYSTATE(currHW, currDB, currSW);
}

static void updateActiveUsbReports(void)
{
    clearActiveReports();
    HardwareModifierStatePrevious = HardwareModifierState;
    HardwareModifierState = 0;
    SuppressMods = false;
    SuppressKeys = false;


    if (MacroPlaying) {
        Macros_ContinueMacro();
    }

    /* TODO: enable this again?
    if(!PostponeKeys || Postponer_Overflowing()) {
        Postponer_RunPostponed();
    }*/

    memcpy(activeMouseStates, toggledMouseStates, ACTIVE_MOUSE_STATES_COUNT);

    layer_id_t activeLayer = LayerId_Base;
    if (secondaryRoleState == SecondaryRoleState_Triggered && IS_SECONDARY_ROLE_LAYER_SWITCHER(secondaryRole)) {
        activeLayer = SECONDARY_ROLE_LAYER_TO_LAYER_ID(secondaryRole);
    }
    if (activeLayer == LayerId_Base) {
        activeLayer = GetActiveLayer();
    }
    bool layerChanged = previousLayer != activeLayer;
    if (layerChanged) {
        stickyModifiers = 0;
    }
    LedDisplay_SetLayer(activeLayer);

    if (TestUsbStack) {
        static bool simulateKeypresses, isEven, isEvenMedia;
        static uint32_t mediaCounter = 0;
        key_state_t *testKeyState = &KeyStates[SlotId_LeftKeyboardHalf][0];

        if (activeLayer == LayerId_Fn && ACTIVATED_NOW(testKeyState)) {
            simulateKeypresses = !simulateKeypresses;
        }
        if (simulateKeypresses) {
            isEven = !isEven;
            ActiveUsbBasicKeyboardReport->scancodes[basicScancodeIndex++] = isEven ? HID_KEYBOARD_SC_A : HID_KEYBOARD_SC_BACKSPACE;
            if (++mediaCounter % 200 == 0) {
                isEvenMedia = !isEvenMedia;
                ActiveUsbMediaKeyboardReport->scancodes[mediaScancodeIndex++] = isEvenMedia ? MEDIA_VOLUME_DOWN : MEDIA_VOLUME_UP;
            }
            MouseMoveState.xOut = isEven ? -5 : 5;
        }
    }

    for (uint8_t slotId=0; slotId<SLOT_COUNT; slotId++) {
        for (uint8_t keyId=0; keyId<MAX_KEY_COUNT_PER_MODULE; keyId++) {
            key_state_t *keyState = &KeyStates[slotId][keyId];
            key_action_t *action;

            preprocessKeyState(keyState);

            if (ACTIVATED_NOW(keyState)) {
                if (SleepModeActive) {
                    WakeUpHost();
                }
                if (secondaryRoleState == SecondaryRoleState_Pressed) {
                    // Trigger secondary role.
                    secondaryRoleState = SecondaryRoleState_Triggered;
                    keyState->current = 0;
                } else {
                    actionCache[slotId][keyId] = CurrentKeymap[activeLayer][slotId][keyId];
                }
            }

            action = &actionCache[slotId][keyId];

            bool isKeystrokeActivatedOnRelease = ActivateOnRelease && action->type == KeyActionType_Keystroke && action->keystroke.modifiers == 0 && action->keystroke.secondaryRole == 0;
            if ( isKeystrokeActivatedOnRelease ) {
                if(DEACTIVATED_NOW(keyState)) {
                    applyKeyAction(keyState, action, slotId, keyId);
                }
            }
            else if (ACTIVE(keyState)) {
                if (action->type == KeyActionType_Keystroke && action->keystroke.secondaryRole) {
                    // Press released secondary role key.
                    if (ACTIVATED_NOW(keyState) && secondaryRoleState == SecondaryRoleState_Released) {
                        secondaryRoleState = SecondaryRoleState_Pressed;
                        secondaryRoleSlotId = slotId;
                        secondaryRoleKeyId = keyId;
                        secondaryRole = action->keystroke.secondaryRole;
                    }
                } else {
                    applyKeyAction(keyState, action, slotId, keyId);
                }
            } else {
                // Release secondary role key.
                if (DEACTIVATED_NOW(keyState) && secondaryRoleSlotId == slotId && secondaryRoleKeyId == keyId && secondaryRoleState != SecondaryRoleState_Released) {
                    // Trigger primary role.
                    if (secondaryRoleState == SecondaryRoleState_Pressed) {
                        ActivateKey(keyState, true);
                        applyKeyAction(keyState, action, slotId, keyId);
                    }
                    secondaryRoleState = SecondaryRoleState_Released;
                } else {
                    applyKeyAction(keyState, action, slotId, keyId);
                }
            }
        }
    }

    PostponerCore_FinishCycle();

    mergeReports();

    processMouseActions();

    // When a layer switcher key gets pressed along with another key that produces some modifiers
    // and the accomanying key gets released then keep the related modifiers active a long as the
    // layer switcher key stays pressed.  Useful for Alt+Tab keymappings and the like.
    if (StickyModifiersEnabled) {
        ActiveUsbBasicKeyboardReport->modifiers |= stickyModifiers;
    }

    if (secondaryRoleState == SecondaryRoleState_Triggered && IS_SECONDARY_ROLE_MODIFIER(secondaryRole)) {
        ActiveUsbBasicKeyboardReport->modifiers |= SECONDARY_ROLE_MODIFIER_TO_HID_MODIFIER(secondaryRole);
    }

    previousLayer = activeLayer;
}

uint32_t UsbReportUpdateCounter;

void reportReport() {
    Macros_SetStatusString("Reporting ", NULL);
    for ( int i = 0; i < USB_BASIC_KEYBOARD_MAX_KEYS; i++) {
        if(ActiveUsbBasicKeyboardReport->scancodes[i] != 0){
            Macros_SetStatusChar(MacroShortcutParser_ScancodeToCharacter(ActiveUsbBasicKeyboardReport->scancodes[i]));
        }
    }
    Macros_SetStatusString("\n", NULL);
}

void UpdateUsbReports(void)
{
    static uint32_t lastUpdateTime;

    for (uint8_t keyId = 0; keyId < RIGHT_KEY_MATRIX_KEY_COUNT; keyId++) {
        KeyStates[SlotId_RightKeyboardHalf][keyId].current = (KeyStates[SlotId_RightKeyboardHalf][keyId].current & ~KeyState_Hw) | (RightKeyMatrix.keyStates[keyId] ? KeyState_Hw : 0);
    }

    if (UsbReportUpdateSemaphore && !SleepModeActive) {
        if (Timer_GetElapsedTime(&lastUpdateTime) < USB_SEMAPHORE_TIMEOUT) {
            return;
        } else {
            UsbReportUpdateSemaphore = 0;
        }
    }

    if(KeystrokeDelay > Timer_GetElapsedTime(&KeystrokeDelayStarted)) {
        return;
    }

    lastUpdateTime = CurrentTime;
    UsbReportUpdateCounter++;

    ResetActiveUsbBasicKeyboardReport();
    ResetActiveUsbMediaKeyboardReport();
    ResetActiveUsbSystemKeyboardReport();
    ResetActiveUsbMouseReport();

    updateActiveUsbReports();

    bool HasUsbBasicKeyboardReportChanged = memcmp(ActiveUsbBasicKeyboardReport, GetInactiveUsbBasicKeyboardReport(), sizeof(usb_basic_keyboard_report_t)) != 0;
    bool HasUsbMediaKeyboardReportChanged = memcmp(ActiveUsbMediaKeyboardReport, GetInactiveUsbMediaKeyboardReport(), sizeof(usb_media_keyboard_report_t)) != 0;
    bool HasUsbSystemKeyboardReportChanged = memcmp(ActiveUsbSystemKeyboardReport, GetInactiveUsbSystemKeyboardReport(), sizeof(usb_system_keyboard_report_t)) != 0;
    bool HasUsbMouseReportChanged = memcmp(ActiveUsbMouseReport, GetInactiveUsbMouseReport(), sizeof(usb_mouse_report_t)) != 0;

    if(HasUsbBasicKeyboardReportChanged || HasUsbMediaKeyboardReportChanged || HasUsbSystemKeyboardReportChanged || HasUsbMouseReportChanged) {
        KeystrokeDelayStarted = CurrentTime;
    }

    if (HasUsbBasicKeyboardReportChanged) {
        MacroRecorder_RecordBasicReport(ActiveUsbBasicKeyboardReport);
#ifdef DEBUG_POSTPONER
        reportReport();
#endif
        if(RuntimeMacroRecordingBlind) {
            //just switch reports without sending the report
            ActiveUsbBasicKeyboardReport = GetInactiveUsbBasicKeyboardReport();
        } else {
            usb_status_t status = UsbBasicKeyboardAction();
            if (status == kStatus_USB_Success) {
                UsbReportUpdateSemaphore |= 1 << USB_BASIC_KEYBOARD_INTERFACE_INDEX;
            }
        }
    }

    if (HasUsbMediaKeyboardReportChanged) {
        usb_status_t status = UsbMediaKeyboardAction();
        if (status == kStatus_USB_Success) {
            UsbReportUpdateSemaphore |= 1 << USB_MEDIA_KEYBOARD_INTERFACE_INDEX;
        }
    }

    if (HasUsbSystemKeyboardReportChanged) {
        usb_status_t status = UsbSystemKeyboardAction();
        if (status == kStatus_USB_Success) {
            UsbReportUpdateSemaphore |= 1 << USB_SYSTEM_KEYBOARD_INTERFACE_INDEX;
        }
    }

    // Send out the mouse position and wheel values continuously if the report is not zeros, but only send the mouse button states when they change.
    if (HasUsbMouseReportChanged || ActiveUsbMouseReport->x || ActiveUsbMouseReport->y ||
            ActiveUsbMouseReport->wheelX || ActiveUsbMouseReport->wheelY) {
        usb_status_t status = UsbMouseAction();
        if (status == kStatus_USB_Success) {
            UsbReportUpdateSemaphore |= 1 << USB_MOUSE_INTERFACE_INDEX;
        }
    }
}

/*
 * Features - upstream features this fork does not ship yet.  False keeps stock behaviour.
 *
 * PHONE_DATA matches the hook's HOOK_PHONE_DATA build switch (scripts/build_hook_common.sh):
 * CarPlay calls, lists and phone battery on the cluster telephone tab (GatedPhoneService,
 * GatedPhoneListener, GatedAddressBook) and vehicle data to the phone (VehicleDataModule).
 * While it is false the stock phone services are registered unwrapped and the vehicle
 * module is not started.
 *
 * UPMIX matches HOOK_AUDIO: the "CarPlay UpMix" row on Sound -> Sound effects (tone
 * Activator, UpmixToneUi) and its stored level (UpmixSetting).
 *
 * SIRI_SHORT_PRESS: a short press of the wheel voice key starts Siri instead of the car's
 * voice control (TerminalModeSDSHandler, CarPlaySpeechRequestHandler).
 *
 * CARPLAY_POPUP_SKIP: entering the phone app presses "Switch to Apple CarPlay" on the
 * "Your iPhone is in Apple CarPlay mode" popup (CarPlayPhoneSMM, CarPlaySystemSMM).
 */
package com.luka.carplay.core;

public final class Features {
    public static final boolean PHONE_DATA = false;
    public static final boolean UPMIX = false;
    public static final boolean SIRI_SHORT_PRESS = false;
    public static final boolean CARPLAY_POPUP_SKIP = false;

    private Features() { }
}

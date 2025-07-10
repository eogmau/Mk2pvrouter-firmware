# Mk2pvrouter-firmware

# Mk2_bothDisplays_4_1290.ino
February 2023: updated to Mk2_bothDisplays_4_1290, with this change:
- increase the working range to 8Wh with the "mid-point" energy level at the 7.5 Wh mark. 
This is to delay start-up while other control systems respond to changing conditions, example storage battery exist and diverter takes power from battery.
When an export situation ends (i.e. no loads able to consume power), the Mk2 will deactivate its load after the normal (0.5 Wh) delay.

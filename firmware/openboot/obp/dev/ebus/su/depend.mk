# Copyright (C) 2026 Andy Taylor. Distributed under the same terms as the
# OpenBoot release it is part of (see LICENSE).
su.fc: ${BP}/dev/serial/mode-data.fth
su.fc: ${BP}/dev/serial/mode-decode.fth
su.fc: ${BP}/dev/serial/ttydriver.fth
su.fc: ${BP}/dev/ebus/su/io.fth
su.fc: ${BP}/dev/ebus/su/su.tok
kbd.fc: ${BP}/dev/serial/mode-data.fth
kbd.fc: ${BP}/dev/serial/mode-decode.fth
kbd.fc: ${BP}/dev/serial/ttydriver.fth
kbd.fc: ${BP}/dev/serial/keyboard/buffer.fth
kbd.fc: ${BP}/dev/serial/keyboard/options.fth
kbd.fc: ${BP}/dev/serial/keyboard/mutex.fth
kbd.fc: ${BP}/pkg/keyboard/keycodes.fth
kbd.fc: ${BP}/dev/usb-devices/kbd/usbdefs.fth
kbd.fc: ${BP}/dev/usb-devices/kbd/usbkeyin.fth
kbd.fc: ${BP}/dev/usb-devices/kbd/kbdutils.fth
kbd.fc: ${BP}/dev/ebus/su/io.fth
kbd.fc: ${BP}/dev/ebus/su/kbd.fth
kbd.fc: ${BP}/dev/ebus/su/kbd.tok

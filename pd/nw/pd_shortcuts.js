"use strict";

var is_webapp = typeof process === "undefined";
var is_mac = is_webapp ? navigator.platform.toUpperCase().indexOf("MAC") > -1 : process.platform === "darwin";
var cmd_or_ctrl = is_mac ? "Cmd" : "Ctrl";


exports.menu = {
  "new":   { key: "N", modifiers: cmd_or_ctrl },
  "open":   { key: "O", modifiers: cmd_or_ctrl },
  "save":   { key: "S", modifiers: cmd_or_ctrl },
  "saveas": { key: "S", modifiers: cmd_or_ctrl + "+Shift" },
  "print":  { key: "P", modifiers: cmd_or_ctrl + "+Shift" },
  "message" : { key: "M", modifiers: cmd_or_ctrl },
  "close":  { key: "W", modifiers: cmd_or_ctrl },
  "quit":   { key: "Q", modifiers: cmd_or_ctrl },

  "undo":   { key: "Z", modifiers: cmd_or_ctrl },
  "redo":   { key: "Z", modifiers: cmd_or_ctrl + "+Shift" },
  "selectall":{ key: "A", modifiers: cmd_or_ctrl },
  "cut":    { key: "X", modifiers: cmd_or_ctrl },
  "copy":   { key: "C", modifiers: cmd_or_ctrl },
  "paste":  { key: "V", modifiers: cmd_or_ctrl },
  "paste_clipboard": { key: "V", modifiers: cmd_or_ctrl + "+Alt" },
  "duplicate": { key: "D", modifiers: cmd_or_ctrl },
  "undo":   { key: "Z", modifiers: cmd_or_ctrl },

  "reselect": { key: String.fromCharCode(10), modifiers: cmd_or_ctrl },
  "clear_console": { key: "L", modifiers: cmd_or_ctrl + "+Shift" },
  "tidyup": { key: "Y", modifiers: cmd_or_ctrl },
  "cordinspector":   { key: "R", modifiers: cmd_or_ctrl + "+Shift" },
  "find":   { key: "F", modifiers: cmd_or_ctrl },
  "findagain":{ key: "G", modifiers: cmd_or_ctrl },
  "editmode": { key: "E", modifiers: cmd_or_ctrl },
  "preferences": { key: (process.platform === "darwin") ? "," : "P",
    modifiers: cmd_or_ctrl },

  "zoomin": { key: "=", modifiers: cmd_or_ctrl },
  "zoomout": { key: "-", modifiers: cmd_or_ctrl },
  "zoomreset": { key: "0", modifiers: cmd_or_ctrl },
  "zoomoptimal": { key: "9", modifiers: cmd_or_ctrl },
  "zoomhoriz": { key: "9", modifiers: cmd_or_ctrl + "+Alt" },
  "zoomvert": { key: "9", modifiers: cmd_or_ctrl + "+Shift" },
  "fullscreen": { key: (process.platform === "darwin") ? "F" : "F11",
    modifiers: (process.platform === "darwin") ? "Cmd+Ctrl" : null },

  "object": { key: "1", modifiers: cmd_or_ctrl },
  "msgbox": { key: "2", modifiers: cmd_or_ctrl },
  "number": { key: "3", modifiers: cmd_or_ctrl },
  "symbol": { key: "4", modifiers: cmd_or_ctrl },
  "comment": { key: "5", modifiers: cmd_or_ctrl },
  "dropdown": { key: "6", modifiers: cmd_or_ctrl },
  "bang": { key: "B", modifiers: cmd_or_ctrl + "+Shift" },
  "toggle": { key: "T", modifiers: cmd_or_ctrl + "+Shift" },
  "number2": { key: "N", modifiers: cmd_or_ctrl + "+Shift" },
  "vslider": { key: "V", modifiers: cmd_or_ctrl + "+Shift" },
  "hslider": { key: "H", modifiers: cmd_or_ctrl + "+Shift" },
  "vradio": { key: "D", modifiers: cmd_or_ctrl + "+Shift" },
  "hradio": { key: "I", modifiers: cmd_or_ctrl + "+Shift" },
  "vu":     { key: "U", modifiers: cmd_or_ctrl + "+Shift" },
  "cnv": { key: "C", modifiers: cmd_or_ctrl + "+Shift" },

  "nextwin": { key: "PageDown", modifiers: cmd_or_ctrl },
  "prevwin": { key: "PageUp", modifiers: cmd_or_ctrl },
  "pdwin": { key: "R", modifiers: cmd_or_ctrl },

  "audio_on": { key: "/", modifiers: cmd_or_ctrl },
  "audio_off": { key: ".", modifiers: cmd_or_ctrl },

  "browser": { key: "B", modifiers: cmd_or_ctrl },
  "audio_off": { key: ".", modifiers: cmd_or_ctrl },
  "audio_off": { key: ".", modifiers: cmd_or_ctrl },
  "audio_off": { key: ".", modifiers: cmd_or_ctrl },


  // Webapp shortcuts
  "new_web":   { key: "N", modifiers: cmd_or_ctrl },
  "open_web":   { key: "O", modifiers: cmd_or_ctrl },
  "save_web":   { key: "S", modifiers: cmd_or_ctrl },
  "saveas_web": { key: "S", modifiers: cmd_or_ctrl + "+Shift" },
  "print_web":  { key: "P", modifiers: cmd_or_ctrl },
  "message_web" : { key: "M", modifiers: cmd_or_ctrl },
  "close_web":  { key: "W", modifiers: cmd_or_ctrl },

  "undo_web":   { key: "Z", modifiers: cmd_or_ctrl },
  "redo_web":   { key: "Z", modifiers: cmd_or_ctrl + "+Shift" },
  "selectall_web":{ key: "Q", modifiers: cmd_or_ctrl + "+Shift"},
  "cut_web":    { key: "X", modifiers: cmd_or_ctrl + "+Shift" },
  "copy_web":   { key: "C", modifiers: cmd_or_ctrl },
  "paste_web":  { key: "V", modifiers: cmd_or_ctrl },
  "paste_clipboard_web": { key: "G", modifiers: cmd_or_ctrl + "+Shift" },
  "duplicate_web": { key: "D", modifiers: cmd_or_ctrl },

  "reselect_web": { key: "Q", modifiers: cmd_or_ctrl + "+Shift" },
  "clear_console_web": { key: "L", modifiers: cmd_or_ctrl + "+Shift" },
  "tidyup_web": { key: "Y", modifiers: cmd_or_ctrl },
  "cordinspector_web":   { key: "R", modifiers: cmd_or_ctrl + "+Shift" },
  "find_web":   { key: "F", modifiers: cmd_or_ctrl },
  "findagain_web":{ key: "F", modifiers: cmd_or_ctrl + "+Shift"},
  "editmode_web": { key: "E", modifiers: cmd_or_ctrl + "+Shift" },
  "preferences_web": { key: (is_mac) ? "," : "P",
    modifiers: cmd_or_ctrl },

  "zoomin_web": { key: "=", modifiers: cmd_or_ctrl },
  "zoomout_web": { key: "-", modifiers: cmd_or_ctrl },
  "zoomreset_web": { key: "0", modifiers: cmd_or_ctrl },
  "zoomoptimal_web": { key: "9", modifiers: cmd_or_ctrl },
  "zoomhoriz_web": { key: "9", modifiers: cmd_or_ctrl },
  "zoomvert_web": { key: "9", modifiers: cmd_or_ctrl + "+Shift" },
  "fullscreen_web": { key: (is_mac) ? "F" : "F11",
    modifiers: (is_mac) ? "Cmd+Ctrl" : null },

  "object_web": { key: "1", modifiers: cmd_or_ctrl },
  "msgbox_web": { key: "2", modifiers: cmd_or_ctrl },
  "number_web": { key: "3", modifiers: cmd_or_ctrl },
  "symbol_web": { key: "4", modifiers: cmd_or_ctrl },
  "comment_web": { key: "5", modifiers: cmd_or_ctrl },
  "dropdown_web": { key: "M", modifiers: cmd_or_ctrl + "+Shift" },
  "bang_web": { key: "B", modifiers: cmd_or_ctrl + "+Shift" },
  "toggle_web": { key: "T", modifiers: cmd_or_ctrl + "+Shift" },
  "number2_web": { key: "N", modifiers: cmd_or_ctrl + "+Shift" },
  "vslider_web": { key: "V", modifiers: cmd_or_ctrl + "+Shift" },
  "hslider_web": { key: "H", modifiers: cmd_or_ctrl + "+Shift" },
  "vradio_web": { key: "D", modifiers: cmd_or_ctrl + "+Shift" },
  "hradio_web": { key: "I", modifiers: cmd_or_ctrl + "+Shift" },
  "vu_web":     { key: "U", modifiers: cmd_or_ctrl + "+Shift" },
  "cnv_web": { key: "C", modifiers: cmd_or_ctrl + "+Shift" },

  "nextwin_web": { key: "PageDown", modifiers: cmd_or_ctrl },
  "prevwin_web": { key: "PageUp", modifiers: cmd_or_ctrl },
  "pdwin_web": { key: "R", modifiers: cmd_or_ctrl },

  "audio_on_web": { key: "/", modifiers: cmd_or_ctrl },
  "audio_off_web": { key: ".", modifiers: cmd_or_ctrl },

  "browser_web": { key: "B", modifiers: cmd_or_ctrl },

}

# FAQ

### Why doesn't it paste when I select an item in history?

1. Make sure *Paste automatically* is enabled in *Settings → General*, or use <kbd>Alt</kbd> + <kbd>Enter</kbd>.
2. Windows does not let normal apps send keystrokes to apps running as administrator. If the target
   app is elevated, the item is still copied, so press <kbd>Ctrl</kbd> + <kbd>V</kbd> yourself.

### The shortcut to open Locket doesn't work

Another app has probably registered the same shortcut. Locket shows a warning in *Settings → General*.
Pick another shortcut, or enable *Replace Win+V*.

### How do I restore the hidden footer?

Open Locket and press <kbd>Ctrl</kbd> + <kbd>,</kbd>, or choose *Settings…* from the notification-area
icon's menu, then enable *Show footer* in *Appearance*.

### How does Locket update?

The Microsoft Store updates Store builds automatically. GitHub builds check GitHub once a day when
*Check for updates automatically* is on (*Settings → General*); *Check now* (in *About*) checks right
away. When a validated new release is available, Locket opens its GitHub release page in your browser
so you can download the package for your PC and run `Install.cmd`. You can skip a version from the
notification.

### Where is my history stored?

Store history is `%LOCALAPPDATA%\Packages\MarshallHD.Locket_*\LocalState\history.sqlite` and is
removed on uninstall. Unpackaged builds use `%LOCALAPPDATA%\Locket\history.sqlite`.

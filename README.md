# GLPI Agent Monitor

[![GLPI Agent Monitor CI](https://github.com/glpi-project/glpi-agentmonitor/actions/workflows/glpi-agentmonitor-ci.yml/badge.svg)](https://github.com/glpi-project/glpi-agentmonitor/actions/workflows/glpi-agentmonitor-ci.yml)

## Description

GLPI Agent Monitor is a simple monitoring tool for GLPI Agent on Windows.

It sits on the system tray as the GLPI Agent logo. The logo changes color
from blue to red depending on the Agent status.
 - As of now, it will only go red if the Agent service is not running.

By default, the tool will start minimized to the system tray, but a
window will be opened if you left-click the icon.

This window shows details such as:
 - Installed GLPI Agent version
 - Agent service status
 - Service startup type
 
Also, it displays the current Agent status (as it would be shown on the /status page).
The status is updated every 2 seconds when the window is open.

Additionaly, you can also:
  - Send a "Force inventory" request to the Agent
  - Go directly to the "New ticket" page on the configured GLPI server (with a screenshot automatically captured to the clipboard)
  - View the Agent logs (with the system default .log viewer)

These features are available either in the main window or the system tray icon's right-click menu.

For future release features, read the [Changelog](CHANGES).

## Releases

Official releases are provided by the [glpi-project/glpi-agentmonitor](https://github.com/glpi-project/glpi-agentmonitor) fork.

## License

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

This software is licensed under the terms of GPLv2+, see LICENSE file for
details.

# GLPI Agent Monitor

[![GLPI Agent Monitor CI](actions/workflows/glpi-agentmonitor-ci.yml/badge.svg)](actions/workflows/glpi-agentmonitor-ci.yml)

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

You can also force an inventory by right-clicking the system tray icon and
clicking "Force inventory", or by clicking the "Force inventory" button
on the tool window.

## License

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

This software is licensed under the terms of GPLv2+, see LICENSE file for
details.

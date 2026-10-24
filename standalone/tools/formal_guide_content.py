"""Formal technical document content for the local-ap-v9 project snapshot."""

def build_formal_pages(assets):
    pages = []
    def page(title, subtitle=''):
        elements=[]
        pages.append(dict(title=title,subtitle=subtitle,elements=elements))
        return elements
    def p(e,t): e.append(('p',t))
    def h(e,t): e.append(('h',t))
    def note(e,t): e.append(('note',t))
    def table(e,heads,rows,widths): e.append(('table',heads,rows,widths))
    def pic(e,name,caption): e.append(('pic',str(assets[name]),caption))

    e=page('STR8BAT\nPICO PROGRAMMER','Technical Description and Operating Procedure')
    p(e,'<b>Standalone STM32 Programming System</b>')
    table(e,['Document control','Details'],[
        ['Document ID','STR8BAT-DP-UM-001'],
        ['Revision','1.1'],
        ['Issue date','02 October 2026'],
        ['Applicable programmer','Raspberry Pi Pico 2 W / RP2350'],
        ['Applicable firmware','local-ap-v9'],
        ['Target device','STM32L476JE, 512 KiB main flash'],
        ['Target image version','v2.0.3'],
        ['Document classification','Technical reference and operating procedure']],[.38,.62])
    h(e,'Purpose')
    p(e,'This document defines the system architecture, electrical interfaces, operator controls, status indications, wireless interface and programming-record management of the Str8bat Pico Programmer.')
    h(e,'Applicability')
    p(e,'The operating procedures apply to an assembled programmer with a START push button, external RGB indicator and STM32 target connection. Electrical installation details are provided separately from the operating procedures.')
    h(e,'Revision history')
    table(e,['Revision','Date','Description'],[
        ['1.0','02 October 2026','Initial project guide.'],
        ['1.1','02 October 2026','Formal document structure; button-based operating procedures; consolidated technical reference.']],[.14,.25,.61])
    note(e,'Circuit status: the figures define reference connections consistent with the firmware. Final component selection, MCU package connections and PCB net assignments require verification against the assembled hardware schematic. This document does not constitute PCB design approval.')

    e=page('Contents','Document structure')
    contents=e

    e=page('1. System Description','Purpose, functions and configuration')
    h(e,'1.1 Debug probe function')
    p(e,'A debug probe provides a communication interface between a host controller and a target microcontroller through its debug port. The Str8bat implementation uses Serial Wire Debug (SWD) to identify, reset, halt, program and verify an STM32 target.')
    h(e,'1.2 Standalone programmer')
    p(e,'The Pico stores the target firmware internally. A programming cycle is initiated using the START button. During the cycle, the system validates the target, erases main flash, writes the firmware image and verifies the written data. The result is indicated by the LEDs and recorded in a CSV file.')
    p(e,'A local Wi-Fi access point provides a browser interface for clock synchronization and CSV management. A computer, router and Internet connection are not required for routine programming.')
    h(e,'1.3 System configuration')
    table(e,['Parameter','Specification'],[
        ['Programmer controller','Raspberry Pi Pico 2 W, RP2350'],
        ['Target controller','STM32L476JE'],
        ['Target main-flash capacity','512 KiB (524288 bytes)'],
        ['Programming interface','SWD: SWCLK, SWDIO and separate NRST'],
        ['Reference target supply','Regulated 1.8 V'],
        ['Pico logic supply','3.3 V'],
        ['Level translation','TXS0102 for SWCLK and SWDIO'],
        ['Programming trigger','START push button'],
        ['Record storage','Local Pico flash filesystem'],
        ['Record timestamp zone','India Standard Time (IST, UTC+05:30)']],[.41,.59])
    h(e,'1.4 Operating boundaries')
    p(e,'The documented profile provides local programming and log management. It does not provide a web-based programming trigger, browser firmware upload, Internet access or cloud upload. The current build retains the STM32 in the halted state after successful verification.')

    e=page('2. System Architecture','Functional block diagram')
    pic(e,'architecture','Figure 1. Functional architecture of the standalone programming system.')
    table(e,['Subsystem','Function'],[
        ['Pico controller','Firmware storage, SWD control, verification, hardware timekeeping and log storage.'],
        ['STM32 target','Receives the programmed image and executes it following target reset.'],
        ['Voltage translator','Transfers SWD signals between the 3.3 V and 1.8 V logic domains. [1]'],
        ['Power subsystem','Supplies the Pico, target board and translator voltage rails.'],
        ['Operator controls','START push button, external RGB indicator and onboard status LED.'],
        ['Wireless interface','Local access point and HTTP page for time synchronization and CSV operations.']],[.29,.71])
    p(e,'The phone supplies the initial time reference. Subsequent programming and timekeeping operate independently of the phone while the Pico remains powered.')

    e=page('3. Electrical Circuit and Interfaces','3.1 SWD, reset and common-ground connections')
    pic(e,'swd_diagram','Figure 2. Programmer-to-target reference circuit for a 1.8 V target.')
    p(e,'The translator B ports operate from the Pico 3.3 V rail; the A ports operate from the target 1.8 V rail. The signal path uses one channel for SWCLK and one bidirectional channel for SWDIO. All subsystems share a common ground. [1]')
    p(e,'NRST is controlled separately. The firmware asserts reset by driving the reset output LOW and releases reset by changing the output to a high-impedance input. A target-side pull-up establishes the released level.')
    note(e,'The STM32 block represents an assembled target board. Package-specific supply pins, decoupling, reset components and boot configuration remain part of the target-board schematic. [3]')

    e=page('3. Electrical Circuit and Interfaces','3.2 Power supply and translator support circuit')
    pic(e,'power_detail','Figure 3. Reference regulator, supply bypass and translator-enable circuit.')
    p(e,'The LDK120C18R provides a regulated 1.8 V output. Its enable input must be asserted for operation. The reference uses 1 µF input and output capacitors; final values and regulator loading shall meet the device specifications. [4]')
    p(e,'TXS0102 VCCA and VCCB are supply inputs. Local bypass capacitors and an OE pull-down establish the required supply and startup conditions. The translator does not generate either voltage rail. [1]')
    note(e,'GP13 is not controlled by the current firmware. Regulator enable shall be provided by the hardware design. The independent enable connection shown in Figure 3 shall not be tied to an existing GPIO-connected net. The 3.3 V regulator part selection and J5 power routing require confirmation against the fitted PCB.')

    e=page('3. Electrical Circuit and Interfaces','3.3 Pin assignment and electrical control')
    table(e,['Pico signal','Header pin','Destination / function'],[
        ['GP2 / SWCLK','4','TXS0102 B1 → A1 → STM32 PA14'],
        ['GP3 / SWDIO','5','TXS0102 B2 ↔ A2 ↔ STM32 PA13'],
        ['GP6 / SWD OE','9','TXS0102 enable for SWCLK/SWDIO'],
        ['GP1 / NRST','2','STM32 NRST with target-side pull-up'],
        ['GND','3 or other GND','Common Pico, translator and target ground'],
        ['3V3 OUT','36','TXS0102 VCCB'],
        ['External 1.8 V','—','Target VDD and TXS0102 VCCA'],
        ['GP18 / START','24','START push-button input, active LOW'],
        ['GP7 / RED','10','External red LED channel'],
        ['GP8 / GREEN','11','External green LED channel'],
        ['GP9 / BLUE','12','External blue LED channel']],[.25,.15,.60])
    p(e,'GPIO designations and physical header pin numbers are distinct. Header positions are based on the Pico 2 W pinout. TXS0102 package pin numbers shall be checked against the selected package. [1, 2]')
    h(e,'Translator-enable sequence')
    p(e,'GP6 is LOW during startup and idle. It is asserted HIGH before SWD communication with a 2 µs settling interval and remains asserted during transmit and receive operations. The firmware deasserts GP6 when releasing the SWD interface; reconnection may include an additional disable/enable cycle.')
    h(e,'Voltage compatibility')
    p(e,'Voltage translation is required for the documented 1.8 V target interface. Direct wiring is applicable only to an electrically compatible target. Pico GPIO uses 3.3 V logic; a 1.8 V return signal does not provide a guaranteed input-HIGH level. [2]')
    h(e,'Unused interfaces')
    p(e,'GP4 is unused in this configuration. GP6 controls the SWD translator, not a reset translator. The current firmware does not provide regulator-enable control through GP13.')

    e=page('4. Operator Controls and LED Circuits','Hardware implementation')
    pic(e,'led_diagram','Figure 4. START input and direct-drive common-cathode RGB reference circuit.')
    pic(e,'driver_diagram','Figure 5. Alternative transistor-driven common-anode RGB circuit, one channel shown.')
    p(e,'The external LED outputs are active HIGH. Direct GPIO drive requires a common-cathode RGB and an individual current-limiting resistor per channel. A common-anode assembly requires the appropriate transistor drivers; the LED supply shall not be connected directly to a Pico GPIO.')
    p(e,'The START button is normally open, with an internal input pull-up and 100 ms debounce. One press initiates one programming cycle. A held button does not queue additional cycles. Figure 5 defines the driver principle; fitted resistor and transistor selections are PCB-specific.')

    e=page('5. Operating Procedure','Routine target programming')
    h(e,'5.1 Preconditions')
    p(e,'The programmer and target shall be connected through the designated fixture or programming connector. Power supplies and electrical interfaces shall conform to Section 3. The required target firmware shall be installed in the programmer.')
    h(e,'5.2 Startup and time initialization')
    table(e,['Step','Operator action','Expected result'],[
        ['1','Switch on the programmer and target supply.','Startup checks complete; ready indication is displayed.'],
        ['2','Connect the mobile device to the programmer Wi-Fi network.','Mobile device joins the local access point.'],
        ['3','Open the programmer webpage.','Clock synchronization occurs automatically if time is unset.'],
        ['4','Confirm the clock-ready message. If required, press Set clock from this phone.','Valid time reference is available for CSV records.']],[.10,.48,.42])
    note(e,'Initial time synchronization is required after each Pico power cycle or application restart for valid CSV timestamps. After synchronization, the mobile device may be disconnected while the Pico remains powered.')
    h(e,'5.3 Programming cycle')
    table(e,['Step','Operator action','Expected result'],[
        ['1','Press and release the START button.','External LED changes to solid blue.'],
        ['2','Wait for the programming cycle to finish.','External LED changes to solid green or solid red.'],
        ['3','For a green result, accept the programming operation.','Firmware verification and result storage are complete.'],
        ['4','For a red result, record the onboard flash count.','Fault category is identified using Section 7.'],
        ['5','For the next target, replace the target using the fixture procedure and press START.','A new programming cycle begins.']],[.10,.48,.42])
    h(e,'5.4 Target state after completion')
    p(e,'The current build leaves the target CPU halted after successful programming. Target reset or a target power cycle is required to execute the application. A target-only reset does not affect the Pico clock.')

    e=page('6. External LED Indications','RGB status definitions')
    table(e,['Indication','System state','Required action'],[
        ['OFF during startup','Initial checks are in progress.','Wait for startup completion.'],
        ['Alternating GREEN / RED','Ready before the first cycle; no Wi-Fi client connected.','Perform initial clock synchronization if required.'],
        ['Flashing GREEN','Ready before the first cycle; Wi-Fi client connected.','Confirm the clock-ready message before recording timed operations.'],
        ['Steady BLUE','Programming in progress.','Maintain target connection and power.'],
        ['Steady GREEN','Programming, verification and log storage successful.','Proceed with the next operation.'],
        ['Steady RED','Programming or log-storage failure.','Record onboard error count and diagnostic report.'],
        ['Flashing RED at startup','Startup validation or initialization failure; START blocked.','Inspect the startup diagnostic report.']],[.25,.44,.31])
    h(e,'6.1 Timing')
    p(e,'Ready-state color alternation and startup fault flashing use 250 ms state intervals. A flashing color therefore has a 250 ms ON period followed by a 250 ms OFF period.')
    h(e,'6.2 Result retention')
    p(e,'The completed-cycle result remains displayed until the next programming attempt. Subsequent Wi-Fi connection changes do not replace the retained green or red result.')
    h(e,'6.3 Interpretation')
    p(e,'A Wi-Fi-connected indication does not confirm clock synchronization. The clock-ready message on the webpage provides the time-status confirmation. A green programming result confirms the completed operation; it does not indicate target application execution in the halt-after-verify build.')

    e=page('7. Onboard LED Indications','Activity and fault-code definitions')
    table(e,['Operating state','LED timing'],[
        ['Idle / successful completion','500 ms ON, 500 ms OFF'],
        ['Programming active','125 ms ON, 125 ms OFF'],
        ['Fault indication','Counted 250 ms ON / 250 ms OFF pulses; approximately 2 s between groups']],[.39,.61])
    table(e,['Count','Fault category','Diagnostic focus'],[
        ['1','Image validation','Embedded image integrity and supported image size.'],
        ['2','SWD communication','Supply, signal continuity, translator enable and reset.'],
        ['3','Target validation','Device identity, flash size and memory mapping.'],
        ['4','Protection / option validation','Protection state and option-byte report.'],
        ['5','Reset / halt','Reset connection and target debug state.'],
        ['6','Timeout','Reported stage and operation deadline.'],
        ['7','Flash operation','Flash erase/program controller status.'],
        ['8','Verification','Readback mismatch address and signal/power integrity.'],
        ['9','Target clock','Clock configuration and target power state.'],
        ['10','Log / storage','Available log capacity and filesystem status.'],
        ['11','Time not initialized','Clock-required configuration only; disabled in this profile.']],[.10,.34,.56])
    p(e,'A fault code identifies a category. Detailed diagnosis requires the reported stage, address and status values. Normal activity flashing is continuous; fault codes repeat in separated groups.')
    p(e,'The onboard LED is controlled through the wireless subsystem. A wireless initialization failure may prevent onboard indication; the external LED and USB diagnostic report remain the applicable indicators.')

    e=page('8. Wireless Connection','Access-point settings and captive-portal behavior')
    table(e,['Parameter','Configured value'],[
        ['SSID','Str8bat_Pico'],['Password','str8bat@pico'],
        ['Local webpage','http://192.168.4.1/'],['Security','WPA2'],
        ['Internet dependency','None']],[.37,.63])
    h(e,'8.1 Connection procedure')
    table(e,['Step','Action'],[
        ['1','Open Wi-Fi settings on the mobile device.'],
        ['2','Select Str8bat_Pico and enter the configured password.'],
        ['3','Select Stay connected or Use without Internet when prompted.'],
        ['4','Open the Wi-Fi sign-in notification. If unavailable, open http://192.168.4.1/ in the browser.'],
        ['5','Confirm that the STM32 programming logs page is displayed.']],[.10,.90])
    h(e,'8.2 Automatic webpage redirection')
    p(e,'The Pico assigns a local IP address through DHCP and provides a local DNS service. HTTP connectivity-check requests are redirected to the programmer page using an HTTP 302 response. The mobile operating system may display the page automatically or present a network sign-in notification.')
    p(e,'Automatic page display depends on the mobile operating system. Manual access through the local HTTP address is supported. Encrypted HTTPS requests are not redirected.')
    h(e,'8.3 Connection recovery')
    p(e,'If the page is unavailable, verify the selected Wi-Fi network and local URL. Mobile-data switching or VPN routing may require temporary adjustment. A full browser should be used if the captive-portal window does not support file downloads.')
    p(e,'The listed credentials are the local-hotspot defaults. Deployment-specific overrides may be configured in wifi_config.local.h.')

    e=page('9. Web Interface','Clock and record-management controls')
    table(e,['Control','Function'],[
        ['Set clock from this phone','Synchronizes the Pico time reference with the mobile device.'],
        ['Download CSV','Downloads the current programming records as programming_logs.csv.'],
        ['Clear downloaded logs','Clears the current records after a completed matching download.'],
        ['Archive links','Downloads retained files from earlier log formats, if present.']],[.36,.64])
    h(e,'9.1 Clock synchronization')
    p(e,'On page load, the browser checks the Pico clock status and automatically synchronizes an unset clock. Manual synchronization is available through the clock button while the programmer is idle.')
    table(e,['Displayed message','Meaning'],[
        ['Clock set to IST (UTC+05:30). Ready.','Synchronization completed.'],
        ['Hardware clock running in IST. Ready.','A valid clock reference is already active.']],[.62,.38])
    h(e,'9.2 CSV download and clearance')
    table(e,['Step','Action'],[
        ['1','Wait for completion of any active programming cycle.'],
        ['2','Press Download CSV and save programming_logs.csv.'],
        ['3','Verify that the downloaded file is available in the mobile device storage.'],
        ['4','Press Clear downloaded logs and confirm the clearance prompt.'],
        ['5','If records have changed since download, download the updated file before repeating clearance.']],[.10,.90])
    p(e,'Clearance requires successful network delivery and a matching log version. Confirmation of the saved mobile file remains an operator responsibility. Clearance retains the CSV header and does not affect target firmware, the clock or archive files.')
    h(e,'9.3 Operation restrictions')
    p(e,'Programming is initiated using the physical START button. Clock updates and log operations are unavailable during programming. A START request can interrupt a CSV download; the download must then be repeated. The web interface does not provide firmware upload.')

    e=page('10. Clock and Timestamp Management','Time source, calculation and retention')
    h(e,'10.1 Initial synchronization')
    p(e,'The mobile browser obtains the device time and transmits UTC Unix seconds to the Pico. The firmware initializes the RP2350 POWMAN millisecond timer from this value and selects a crystal-derived 1 kHz clock source.')
    note(e,'Before the first programming cycle after each Pico startup, the mobile device shall connect to Wi-Fi and open the programmer webpage. Following successful time synchronization, the mobile device may be disconnected. The Pico continues timekeeping while powered.')
    h(e,'10.2 Time calculation')
    p(e,'The timer advances independently of Wi-Fi activity. The current UTC value is derived from the synchronized reference plus the elapsed hardware time. The CSV formatter applies the fixed IST offset of 19,800 seconds.')
    note(e,'UTC time = synchronized UTC reference + elapsed hardware time\nIST timestamp = UTC time + 05:30')
    p(e,'Example: synchronization at 10:00:00 IST followed by a cycle starting 12 minutes 15 seconds later produces a start timestamp of 10:12:15 IST. The mobile device is not required to remain connected during this interval.')
    h(e,'10.3 Retention conditions')
    table(e,['Condition','Timekeeping behavior'],[
        ['Mobile device disconnects','Clock continues while Pico remains powered.'],
        ['Target-only reset / power cycle','No change to the Pico time reference.'],
        ['Pico restart / power interruption','New synchronization is required.'],
        ['Cycle started before synchronization','Permitted in this configuration; timestamp is UNSET.'],
        ['Synchronization after UNSET records','Existing records are unchanged.'],
        ['Incorrect mobile time','Incorrect reference is transferred; resynchronization is required.']],[.43,.57])
    p(e,'CSV date/time is captured at cycle start. Programming duration is measured separately using a monotonic timer. No battery-backed time retention or Internet clock correction is provided; oscillator drift remains possible.')

    e=page('11. Programming Records','CSV format and storage management')
    p(e,'Programming records are stored in the Pico flash filesystem and retained through normal power cycles. The records are independent of the target main flash.')
    table(e,['Column','Definition'],[
        ['Target UID','STM32 96-bit unique identifier represented by 24 hexadecimal digits; UNAVAILABLE when not obtained.'],
        ['Date&Time(IST)','Cycle start timestamp in YYYY-MM-DD HH:MM:SS format; UNSET before synchronization.'],
        ['Elapsed Time(sec)','Programming duration in seconds with three decimal places.'],
        ['firmware_version','Embedded-image build label; v2.0.3 in this configuration.'],
        ['Binary Size(Bytes)','Embedded-image size; 524288 bytes in this configuration.'],
        ['status','SUCCESS or FAIL. Detailed fault information is provided in the diagnostic report.']],[.32,.68])
    h(e,'11.1 Record example')
    table(e,['Field','Illustrative value'],[
        ['Target UID','00112233445566778899AABB'],['Date&Time(IST)','2026-10-02 10:12:15'],
        ['Elapsed Time(sec)','8.450'],['firmware_version','v2.0.3'],['Binary Size(Bytes)','524288'],['status','SUCCESS']],[.40,.60])
    p(e,'Example values are illustrative and do not represent a measured cycle. The UID column should be imported as text in spreadsheet applications to preserve leading zeros.')
    h(e,'11.2 Capacity and interrupted cycles')
    p(e,'The current CSV capacity limit is 256 KiB. Insufficient log capacity blocks a new programming cycle before target erase and generates error code 10. Records shall be downloaded and verified before clearance.')
    p(e,'A persistent pending-cycle marker supports recovery of an interrupted cycle as FAIL. Unavailable fields may be represented by UNAVAILABLE, UNSET or 0.000 for unknown duration. Target programming may be incomplete following power interruption.')
    p(e,'A log-save failure after target verification can produce a red indication and error code 10 even when target flash programming completed.')

    e=page('12. Diagnostics and Technical Reference','Fault reporting and programming policy')
    h(e,'12.1 Diagnostic report')
    p(e,'The USB report identifies the most recent programming result. Repeated report groups describe the retained result rather than separate operations. The report can be collected using the following read-only command from the project directory:')
    p(e,'<b>powershell -ExecutionPolicy Bypass -File .\\read-standalone-report.ps1</b>')
    table(e,['Field','Description'],[
        ['result','Programming result category.'],
        ['stage','0: connection/setup; 1: erase; 2: program; 3: verify; 4: final target state.'],
        ['address / detail','Address and diagnostic value associated with the result.'],
        ['OPTIONS before / after','Observed option-register values; after alone does not confirm a completed reload.'],
        ['bfb2_cleared','Verified BFB2 clearance following option reload when set to 1.'],
        ['halted','Confirmed target state at cycle completion.']],[.31,.69])
    h(e,'12.2 Flash programming policy')
    p(e,'The current embedded image occupies the 512 KiB main-flash range from 0x08000000 to 0x0807FFFF. The operation erases all main flash; existing application data is retained only if included in the source image.')
    table(e,['Region in dual-bank mode','Inclusive address range'],[
        ['Bank 1 — 256 KiB','0x08000000–0x0803FFFF'],
        ['Bank 2 — 256 KiB','0x08040000–0x0807FFFF']],[.46,.54])
    p(e,'BFB2 is a boot-selection option, independent of whether Bank 2 contains data. The firmware clears BFB2 when required and verifies the setting after option reload. Other option settings are preserved; blanket option-byte erase and automatic removal of RDP, WRP or PCROP are not part of the programming policy.')
    h(e,'12.3 Configuration identification')
    p(e,'Programmer build: local-ap-v9. Target image: str8bat_factory_v2.0_user_v2.0.3.bin. The active standalone application is standalone/src/main.c. The documented build enables halt-after-verify. Generated firmware-image sources are derived from the selected BIN.')

    e=page('Appendix A. Source References','Firmware traceability and hardware documentation')
    table(e,['Source under standalone/','Documented function'],[
        ['include/programmer_config.h','GPIO assignment and target policy.'],
        ['include/wifi_config.h','Local access-point settings and clock requirement.'],
        ['src/main.c','START processing, LED states and result handling.'],
        ['src/swd.c','SWD interface, reset and translator-enable control.'],
        ['src/stm32_programmer.c','Target identification, options, erase, programming and verification.'],
        ['include/stm32_programmer.h','Result categories and report fields.'],
        ['src/services/local_portal.c','Web controls, redirection, download and clearance.'],
        ['src/services/local_clock.c','Hardware time reference and IST formatting.'],
        ['src/services/local_storage.c','CSV format, capacity and interrupted-cycle recovery.']],[.51,.49])
    h(e,'Hardware references')
    p(e,'<b>[1] Texas Instruments — TXS0102 datasheet.</b> Supply rails, OE behavior and package pin assignments.\nhttps://www.ti.com/lit/ds/symlink/txs0102.pdf')
    p(e,'<b>[2] Raspberry Pi — Pico 2 W datasheet.</b> Header assignment and electrical characteristics.\nhttps://datasheets.raspberrypi.com/picow/pico-2-w-datasheet.pdf')
    p(e,'<b>[3] STMicroelectronics — STM32L476xx datasheet.</b> Target interfaces and supply requirements.\nhttps://www.st.com/resource/en/datasheet/stm32l476je.pdf')
    p(e,'<b>[4] STMicroelectronics — LDK120 datasheet.</b> Regulator enable and capacitor requirements.\nhttps://www.st.com/resource/en/datasheet/ldk120.pdf')
    h(e,'Reference limitations')
    p(e,'Firmware behavior is based on the reviewed source and build configuration. The circuit figures document functional reference connections. Final PCB component values, regulator selection, LED assembly and MCU package routing remain subject to hardware verification.')

    # Fixed section starts; actual Word pagination is checked before delivery.
    table(contents,['Section','Page'],[(pg['subtitle'] if pg['title'].startswith('3. Electrical') else pg['title'].replace('\n',' '),str(i+1)) for i,pg in enumerate(pages) if i>1],[.88,.12])
    h(contents,'Abbreviations')
    table(contents,['Term','Definition'],[
        ['SWD','Serial Wire Debug'],['OE','Output Enable'],['NRST','Active-low reset'],
        ['CSV','Comma-Separated Values'],['IST','India Standard Time'],
        ['UTC','Coordinated Universal Time'],['GPIO','General-Purpose Input/Output']],[.22,.78])
    return pages

# RDM-7 Digital Dashboard User Guide

Welcome to the **RDM-7 Digital Dashboard**. This guide will help you get your dash wired, configured, and customized for your vehicle.

---

## 🔌 1. Wiring & Installation

The RDM-7 typically requires just 4 wire connections to get running.

| Wire Color | Function | Connection |
| :--- | :--- | :--- |
| **🔴 Red** | **+12V Power** | Switched Ignition Source (12V) |
| **⚫ Black** | **Ground** | Clean Chassis Ground or Battery Negative |
| **🟡 Yellow** | **CAN Low** | ECU CAN Low |
| **🟢 Green** | **CAN High** | ECU CAN High |

> [!TIP]
> **Can't find your CAN wires?** Check your ECU wiring diagram for "CAN H" and "CAN L". Twisted pair wiring is recommended for the best signal quality.

---

## ⚙️ 2. First Time Setup

Once wired, power on the dashboard. You will need to configure it to talk to your specific ECU.

### **Step 1: Enter Device Settings**
1.  **Long press** the **RDM Logo** on the home screen.
2.  The **Device Settings** menu will appear.

![Screenshot: Device Settings Menu]

### **Step 2: Configure CAN Bus**
1.  Locate the **"CAN BUS"** section in settings.
2.  Select your **Bitrate** from the dropdown (e.g., `500 kbps`, `1 Mbps`).
    *   *Most aftermarket ECUs use 500 kbps or 1 Mbps.*

### **Step 3: Select ECU Pre-Config**
1.  Scroll to the **"ECU Pre-Config"** section at the bottom.
2.  Select the **ECU Type** dropdown:
    *   **MaxxECU**
    *   **Haltech**
    *   **Ford** (BA/BF/FG)
3.  Select your specific **Version** if prompted (e.g., "Nexus" for Haltech, "1.2/1.3" for MaxxECU).
4.  The dashboard will automatically save your selection.
5.  Press **"X"** to return to the home screen.

---

## 🎨 3. Customizing the Display

Now that the dash is communicating with your ECU, you can customize what data is shown.

### **1. RPM (Tachometer)**
*   **Long press** the RPM gauge area.
*   **Settings:**
    *   **RPM Colour:** Select the main color of the RPM bar.
    *   **Redline RPM:** Set the RPM where the redline zone begins.
    *   **RPM Limiter:** Set the RPM threshold for the limiter warning.
    *   **Limiter Effect:** Choose the visual effect when the limiter is hit (e.g., Bar Flash, Circles Flash).
    *   **Limiter Colour:** Select the color for the limiter effect.
    *   **RPM Lights:** Toggle the circular shift/warning lights on or off.
    *   **RPM Background:** Toggle the secondary background bar on or off.
    *   **Background Colour:** Select the color for the background bar.
    *   **Background RPM:** Set the RPM threshold for the background bar.

### **2. Speedometer**
*   **Long press** the central Speed reading.
*   **Settings:**
    *   Use **ECU Pre-Config** to select **"Vehicle Speed"** (or similar) for your ECU.
    *   Alternatively, manually enter the CAN ID and Bit configurations.
    *   **Speed Units:** Select **KMH** or **MPH**.

### **3. Bar Gauges (Left & Right)**
*   **Long press** the Left or Right vertical bar.
*   **Settings:**
    *   Select the **Sensor** to display (e.g., Throttle, Boost, Coolant Temp).
    *   Set **Min/Max** range values.
    *   Configure **Low/High Warning Colors** (e.g., Blue for cold, Red for hot).

### **4. Data Panels (8 Blocks)**
*   **Long press** any of the 8 rectangular data blocks.
*   **Settings:**
    *   **Data Source:** Choose the sensor value (RPM, TPS, MAP, Lambda, etc.).
    *   **Label:** Rename the panel (e.g., "Oil P", "Boost").
    *   **Display Unit:** Add a custom unit suffix (e.g., "psi", "°C", "%").
    *   **Decimals:** Adjust how many decimal places to show.
    *   **Range Low:** Set a value threshold; if the reading drops **below** this, the panel will highlight selected colour
     (e.g., RED for Oil Pressure or BLUE for Coolant Temp).
    *   **Range High:** Set a value threshold; if the reading goes **above** this, the panel will highlight selected colour (e.g., RED for Coolant Temp or Lambda).

![Screenshot: Value Configuration Menu]

---

## 🌐 5. WiFi & Updates

To keep your dashboard running with the latest features, connect it to WiFi.

### **Connecting to WiFi**
1.  Go to **Device Settings** (Long press RDM Logo).
2.  Scroll to the **"NETWORK & UPDATES"** section.
3.  Click **"Wi-Fi Settings"**.
4.  Wait for the network scan to complete.
5.  Select your network from the list.
6.  Enter your password using the on-screen keyboard and press **"Connect"**.
7.  Once connected, the status will show "Connected" and display your IP address.

### **Checking for Updates**
1.  Ensure you are connected to WiFi.
2.  In **Device Settings**, click **"Check Updates"**.
3.  If an update is available, a dialog will appear with release notes.
4.  Follow the prompts to download and install the update.
    *   *The dash will restart automatically after the update.*

---
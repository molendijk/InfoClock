#include "Arduino.h"
#include <time.h>
#include "sntp.h"
#include "FS.h"

#include "config.h"
#include "DisplayTask.hpp"
#include "task.hpp"
#include "tasks_utils.h"
#include "utils.h"
#include "DataStore.h"

#include "WifiConnector.h"
#include "WeatherGetter.h"
#include "WebServerTask.h"
#include "LHCStatusReader.h"

#include "MacroStringReplace.h"

using namespace Tasks;

class LedBlinker: public Task
{
	public:
		LedBlinker()
		{
			pinMode(LBLUE, OUTPUT);
		}

		virtual void run()
		{
			digitalWrite(LBLUE, s);
			sleep(s ? 2_s - 1: 1);
			s = !s;
		}

	private:
		bool s = false;
};


void connectionStateChanged(WifiConnector::States state);

WifiConnector wifiConnector(connectionStateChanged);

LedBlinker ledBlinker;
DisplayTask displayTask(DISPLAYS);
WeatherGetter weatherGetter;
WebServerTask webServerTask;
LHCStatusReader lhcStatusReader;


void connectionStateChanged(WifiConnector::States state)
{
	switch (state)
	{
		case WifiConnector::States::NONE:
			weatherGetter.suspend();
			webServerTask.suspend();
			lhcStatusReader.suspend();
			displayTask.suspend();
			displayTask.pushMessage(F("Initializing..."), 15_s);
			return;

		case WifiConnector::States::AP:
		{
			webServerTask.reset();
			webServerTask.resume();
			displayTask.suspend();

			displayTask.pushMessage(F("AP mode"), 5_s);
			String ip = WiFi.softAPIP().toString();
			DataStore::value("ip") = ip;

			logPrintf("IP = %s", ip.c_str());
			return;
		}

		case WifiConnector::States::CLIENT:
		{
			weatherGetter.reset();
			weatherGetter.resume();

			webServerTask.reset();
			webServerTask.resume();

			lhcStatusReader.reset();
			lhcStatusReader.resume();

			displayTask.resume();

			displayTask.pushMessage(F("Connected to WiFi"), 5_s);
			String ip = WiFi.localIP().toString();
			DataStore::value("ip") = ip;
			logPrintf("IP = %s", ip.c_str());
			break;
		}
	}
}


void setup()
{
	Serial.begin(115200);

	//the filesystem is not ready yet - format it and save some settings
	if (!SPIFFS.begin())
	{
		SPIFFS.format();
		SPIFFS.begin();
		writeConfig(F("configPassword"), F("password"));
		logPrintf("Formatting filesystem, the default password is %s", readConfig(F("configPassword")).c_str());
	}

	//these tasks are always running
	addTask(&ledBlinker);
	addTask(&wifiConnector);

	//and these need to be suspended
	addTask(&displayTask);
	addTask(&weatherGetter);
	addTask(&webServerTask);
	addTask(&lhcStatusReader);

	weatherGetter.suspend();
	webServerTask.suspend();
	lhcStatusReader.suspend();

	setupTasks();

	String macAddress = WiFi.macAddress();
	DataStore::value("mac") = macAddress;
	logPrintf("MAC Address: %s", macAddress.c_str());

	configTime(getTimeZone(), 0, "pool.ntp.org", "time.nist.gov", "ntp3.pl");
}

void loop()
{
	scheduleTasks();
}

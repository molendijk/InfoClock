/*
 * WeatherGetter.cpp
 *
 *  Created on: 04.01.2017
 *      Author: Bartosz Bielawski
 */

#include "WeatherGetter.h"
#include "DataStore.h"
#include "config.h"
#include "utils.h"
#include "tasks_utils.h"
#include "MapCollector.hpp"
#include "WebServerTask.h"
#include "web_utils.h"
#include "CounterCRTP.hpp"

/*
 * 2660646 - Geneva
 * 2659667 - Meyrin
 * 2974936 - Sergy
 * 6424612 - Ornex
 */

using namespace std;

const static char urlTemplate[] PROGMEM = "http://api.openweathermap.org/data/2.5/weather?%s&APPID=%s&units=metric";

static const vector<String> prefixes{
	"main",
	"name"
};

bool jsonPathFilter(const string& key, const string& /*value*/)
{
	String s(key.c_str());

	for (auto& prefix: prefixes)
	{
		//"/root/" => first 6 chars
		if (s.startsWith(prefix, 6))
			return true;
	}

	return false;
}

WeatherGetter::WeatherGetter()
{
	taskName = "WG";
	taskName += counter;

	reset();
	sleep(30_s);
}

void WeatherGetter::reset()
{
	temperature = 0.0f;
	pressure = 0;
	localization = "";
}

void WeatherGetter::run()
{
	String idn(F("owmId"));
	String keyn(F("owmKey"));
	idn += counter;
	keyn += counter;
	auto location = readConfig(idn);
	auto key = readConfig(keyn);

	if (location.length() == 0 or key.length() == 0)
	{
		logPrintfX(taskName, F("Service not configured... "));
		sleep(60_s);
		return;
	}

	char localBuffer[256];
	snprintf_P(localBuffer, sizeof(localBuffer), urlTemplate, location.c_str(), key.c_str());

	logPrintfX(taskName, F("URL = %s"), localBuffer);

	HTTPClient httpClient;
	httpClient.begin(localBuffer);

	int httpCode = httpClient.GET();

	if (httpCode != HTTP_CODE_OK)
	{
		logPrintfX(taskName, F("HTTP failed with code %d"), httpCode);
		sleep(60_s);
		httpClient.end();
		return;
	}

	//fetch the response
	String json = httpClient.getString();

	//prepare the parser - provide filtering function
	MapCollector mc(jsonPathFilter);

	for (size_t  i = 0; i < json.length(); ++i)
	{
		mc.parse(json[i]);
	}

	auto& results = mc.getValues();

	pressure = atoi(results["/root/main/pressure"].c_str());
	temperature = atof(results["/root/main/temp"].c_str());
	localization = results["/root/name"].c_str();

	//print all we have acquired - useful for adding new fields
	for (const auto& e: results)
	{
		logPrintfX(taskName, F("%s = %s"), e.first.c_str(), e.second.c_str());
	}

	httpClient.end();

	String periodParamName(F("owmPeriod"));
	periodParamName += counter;
	int period = readConfig(periodParamName).toInt() * MS_PER_CYCLE;
	if (period == 0)
		period = 600_s;

	sleep(period);
}

static const char owmConfigPage[] PROGMEM = R"_(
<form method="post" action="owmc$n$" autocomplete="on">
<table>
<tr><th>OpenWeatherMap</th></tr>
<tr><td class="l">ID:</td><td><input type="text" name="owmId" value="$owmId$"></td></tr>
<tr><td class="l">Key:</td><td><input type="text" name="owmKey" value="$owmKet$"></td></tr>
<tr><td class="l">Refresh period (s):</td><td><input type="text" name="owmPeriod" value="$owmPeriod$"></td></tr>
<tr><td/><td><input type="submit"></td></tr>
</table></form></body></html>
)_";

FlashStream owmConfigPageFS(owmConfigPage);

void handleOWMConfig(ESP8266WebServer& webServer, void* t)
{
	if (!handleAuth(webServer)) return;

	WeatherGetter* wg = (WeatherGetter*)t;

	auto location = webServer.arg(F("owmId"));
	auto key   = webServer.arg(F("owmKey"));
	auto period = webServer.arg(F("owmPeriod"));

	auto configIdName = String(F("owmId"))+wg->getId();
	auto configKeyName = String(F("owmKey"))+wg->getId();
	auto configPeriodName = String(F("owmPeriod"))+wg->getId();

	if (location.length()) 	writeConfig(configIdName, location);
	if (key.length()) 		writeConfig(configKeyName, key);
	if (period.length())	writeConfig(configPeriodName, period);

	StringStream ss(2048);
	macroStringReplace(pageHeaderFS, constString("OWM Settings"), ss);

	std::map<String, String> m = {
			{F("n"),			String(wg->getId())},
			{F("owmId"), 		readConfig(configIdName)},
			{F("owmKey"), 		readConfig(configKeyName)},
			{F("owmPeriod"), 	readConfig(configPeriodName)},
	};

	macroStringReplace(owmConfigPageFS, mapLookup(m), ss);
	webServer.send(200, textHtml, ss.buffer);
}

static const char owmStatusPage[] PROGMEM = R"_(
<table>
<tr><th>$loc$</th></tr>
<tr><td class="l">Temperature:</td><td>$t$ &#8451;</td></tr>
<tr><td class="l">Pressure:</td><td>$p$ hPa</td></tr>
</table></form></body>
<script>setTimeout(function(){window.location.reload(1);}, 15000);</script>
</html>
)_";

FlashStream owmStatusPageFS(owmStatusPage);


void handleOWMStatus(ESP8266WebServer& webServer, void* t)
{
	WeatherGetter* wg = static_cast<WeatherGetter*>(t);
	StringStream ss(2048);
	macroStringReplace(pageHeaderFS, constString(F("OWM Status")), ss);

	std::map<String, String> m =
	{
			{F("loc"), wg->localization},
			{F("t"), String(wg->temperature)},
			{F("p"), String(wg->pressure)}
	};
	macroStringReplace(owmStatusPageFS, mapLookup(m), ss);
	webServer.send(200, textHtml, ss.buffer);
}

String getWeatherDescription(void* t)
{
	WeatherGetter* wg = static_cast<WeatherGetter*>(t);
	String r(wg->localization);
	r += ": ";
	r += (char)0x82;		//external temperature symbol
	r += String(wg->temperature, 1);
	r += '\x80';
	r += 'C';
	r += " ";
	r += wg->pressure;
	r += " hPa";
	return r;
}

static RegisterPackage r1("OWM1", new WeatherGetter, TaskDescriptor::CONNECTED,
	{
		PageDescriptor("owmc1", "OWM Config 1", &handleOWMConfig),
		PageDescriptor("owms1", "OWM Status 1", &handleOWMStatus)
	},
	{
		{getWeatherDescription, 0.1_s, 1, true}
	}
);

static RegisterPackage r2("OWM2", new WeatherGetter, TaskDescriptor::CONNECTED,
	{
		PageDescriptor("owmc2", "OWM Config 2", &handleOWMConfig),
		PageDescriptor("owms2", "OWM Status 2", &handleOWMStatus)
	},
	{
		{getWeatherDescription, 0.1_s, 1, true}
	}
);


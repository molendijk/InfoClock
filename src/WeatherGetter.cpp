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
#include <MapCollector.hpp>
#include "WebServerTask.h"
#include "web_utils.h"
#include <exception>

#define WIND_ENABLED

/*
 * 2660646 - Geneva
 * 2659667 - Meyrin
 * 2974936 - Sergy
 * 6424612 - Ornex
 */

using namespace std;

const static char urlTemplate[] PROGMEM =
		"http://api.openweathermap.org/data/2.5/weather?id=%d&APPID=%s&units=metric";
const static char urlForecastTemplate[] PROGMEM =
		"http://api.openweathermap.org/data/2.5/forecast?id=%d&APPID=%s&units=metric&cnt=2";


/* forecast path
 /root/list/n/main/temp			--temperature
 /root/list/n/weather/0/main	--description
 /root/list/n/weather/0/description --
 */

static const vector<String> prefixes{
	"main/temp",
	"name",
	"list/1/main/temp",
	"list/1/weather/0/description",
	"wind/speed",
	"wind/deg"
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
	getWebServerTask().registerPage(F("owms"), F("OWM Status"), [this](ESP8266WebServer& ws) {handleStatus(ws);});
	getWebServerTask().registerPage(F("owmc"), F("OWM Config"), [this](ESP8266WebServer& ws) {handleConfig(ws);});

	//TODO: wait, what?
	auto& dt = getDisplayTask();

	getWebServerTask().registerPage(F("dispConf"), F("Display Config"),
		[&dt]  (ESP8266WebServer& ws) mutable {dt.handleConfigPage(ws);});

	getDisplayTask().addRegularMessage({
		this,
		[this](){return getWeatherDescription();},
		0.035_s,
		1,
		true});

	//wait for the network
	suspend();
}

void WeatherGetter::reset()
{
	weathers.clear();

	String ids = readConfig(F("owmId"));
	apiKey = readConfig(F("owmKey"));

	//generate list of "weathers" to be checked
	uint32_t from = 0;
	int32_t to;

	do
	{
		auto commaIndex = ids.indexOf(",", from);
		to = commaIndex == -1 ? ids.length(): commaIndex;

		String s = ids.substring(from, to);
		logPrintfX(F("WG"), "ID: %s", s.c_str());

		from = to+1;

		weathers.emplace_back(s.toInt());
	}
	while (from < ids.length());

	logPrintfX(F("WG"), "Found %d IDs", weathers.size());

	currentWeatherIndex = 0;
}



int getHttpResponse(HTTPClient& httpClient, MapCollector& mc, const char* url)
{
	httpClient.begin(url);
	int httpCode = httpClient.GET();

	if (httpCode != 200)
		return httpCode;

	mc.reset();

	String json = httpClient.getString();
	for (size_t  i = 0; i < json.length(); ++i)
	{
		mc.parse(json[i]);
	}

	httpClient.end();

	return httpCode;
}

#ifdef WIND_ENABLED
char* bearing(float direction)
{
	switch (round( direction / 22.5 ))
	{
		case 0: return (char *)" N";
			break;
		case 1: return (char *)" NNE";
			break;
		case 2: return (char *)" NE";
			break;
		case 3: return (char *)" ENE";
			break;
		case 4: return (char *)" E";
			break;
		case 5: return (char *)" ESE";
			break;
		case 6: return (char *)" SE";
			break;
		case 7: return (char *)" SSE";
			break;
		case 8: return (char *)" S";
			break;
		case 9: return (char *)" SSW";
			break;
		case 10: return (char *)" SW";
			break;
		case 11: return (char *)" WSW";
			break;
		case 12: return (char *)" W";
			break;
		case 13: return (char *)" WNW";
			break;
		case 14: return (char *)" NW";
			break;
		case 15: return (char *)" NNW";
			break;
		case 16: return (char *)" N";
			break;
		default: return (char *)" ?";
	}
}
#endif /WIND_ENABLED



void WeatherGetter::run()
{
	//if no weathers are configured or apiKey is missing...
	if (weathers.size() == 0 or apiKey.length() == 0)
	{
		logPrintfX(F("WG"), F("Service not configured... "));
		sleep(60_s);
		return;
	}

	Weather& w = weathers[currentWeatherIndex];
	logPrintfX(F("WG"), "Reading weather for id = %d", w.locationId);

	currentWeatherIndex++;
	currentWeatherIndex %= weathers.size();

	//locals needed for the rest of the code
	MapCollector mc(jsonPathFilter);
	HTTPClient httpClient;
	httpClient.setReuse(true);
	char localBuffer[256];
	int code = 0;

	while (true)
	{
		//get weather
		snprintf_P(localBuffer, sizeof(localBuffer), urlTemplate, w.locationId, apiKey.c_str());
		logPrintfX(F("WG"), "URL: %s", localBuffer);
		code = getHttpResponse(httpClient, mc, localBuffer);
		if (code != 200)
			break;

		auto& results = mc.getValues();
		w.temperature = atof(results["/root/main/temp"].c_str());
		w.location = results["/root/name"].c_str();
		w.windSpeed = atof(results["/root/wind/speed"].c_str());
		w.windDirection = atoi(results["/root/wind/deg"].c_str());

//		for (const auto& p: results)
//		{
//			logPrintfX(F("WG"), "%s = %s", p.first.c_str(), p.second.c_str());
//		}

		//get forecast
		snprintf_P(localBuffer, sizeof(localBuffer), urlForecastTemplate, w.locationId, apiKey.c_str());
		logPrintfX(F("WG"), "URL: %s", localBuffer);
		code = getHttpResponse(httpClient, mc, localBuffer);
		if (code != 200)
			break;

		w.temperatureForecast = atof(results["/root/list/1/main/temp"].c_str());
		w.description = results["/root/list/1/weather/0/description"].c_str();

//		for (const auto& p: results)
//		{
//			logPrintfX(F("WG"), "%s = %s", p.first.c_str(), p.second.c_str());
//		}


		httpClient.end();

		//oops, forgot to break...
		break;
	}
	if (code != 200)
	{
		logPrintfX(F("WG"), F("HTTP failed with code %d"), code);
		sleep(600_s);
		return;
	}

	if (currentWeatherIndex != 0)
	{
		sleep(5_s);		//one readout every 5 seconds, then longer break...
		return;
	}

	int period = readConfig(F("owmPeriod")).toInt() * 1_s;
	if (period == 0)
		period = 600_s;

	sleep(period);
}

static const char owmConfigPage[] PROGMEM = R"_(
<form method="post" action="owmc" autocomplete="on">
<table>
<tr><th>OpenWeatherMap</th></tr>
<tr><td class="l">ID:</td><td><input type="text" name="owmId" value="$owmId$"></td></tr>
<tr><td class="l">Key:</td><td><input type="text" name="owmKey" value="$owmKet$"></td></tr>
<tr><td class="l">Refresh period (s):</td><td><input type="text" name="owmPeriod" value="$owmPeriod$"></td></tr>
<tr><td/><td><input type="submit"></td></tr>
</table></form></body></html>
)_";

FlashStream owmConfigPageFS(owmConfigPage);

void WeatherGetter::handleConfig(ESP8266WebServer& webServer)
{
	if (!handleAuth(webServer)) return;

	auto location = webServer.arg(F("owmId"));
	auto key   = webServer.arg(F("owmKey"));
	auto period = webServer.arg(F("owmPeriod"));

	auto configIdName = String(F("owmId"));
	auto configKeyName = String(F("owmKey"));
	auto configPeriodName = String(F("owmPeriod"));

	if (location.length()) 	writeConfig(configIdName, location);
	if (key.length()) 		writeConfig(configKeyName, key);
	if (period.length())	writeConfig(configPeriodName, period);

	StringStream ss(2048);
	macroStringReplace(pageHeaderFS, constString("OWM Settings"), ss);

	std::map<String, String> m = {
			{F("owmId"), 		readConfig(configIdName)},
			{F("owmKey"), 		readConfig(configKeyName)},
			{F("owmPeriod"), 	readConfig(configPeriodName)},
	};

	macroStringReplace(owmConfigPageFS, mapLookup(m), ss);
	webServer.send(200, textHtml, ss.buffer);

	//reload display tasks
	reset();
}

static const char owmStatusPage[] PROGMEM = R"_(
<tr><th>$loc$</th></tr>
<tr><td class="l">Temperature:</td><td>$t$ &#8451;</td></tr>
<tr><td class="l">Temperature (forecast):</td><td>$tf$ &#8451;</td></tr>
<tr><td class="l">Weather (forecast):</td><td>$df$</td></tr>
<tr><td class="l">Wind Speed:</td><td>$ws$</td></tr>
<tr><td class="l">Wind Direction:</td><td>$wd$</td></tr>
)_";

static const char owmFooterPage[] PROGMEM = R"_(
</table></form></body>
<script>setTimeout(function(){window.location.reload(1);}, 15000);</script>
</html>
)_";

FlashStream owmStatusPageFS(owmStatusPage);
FlashStream owmFooterPageFS(owmFooterPage);

void WeatherGetter::handleStatus(ESP8266WebServer& webServer)
{
	StringStream ss(2048);
	logPrintfX(F("WG"), "OWM Status...");
	macroStringReplace(pageHeaderFS, constString(F("OWM Status")), ss);
	ss.print("<table>");

	for (auto& w: weathers)
	{
		//don't show it if we didn't get anything yet...
		if (!w.location.length())
			continue;

		std::map<String, String> m =
		{
				{F("loc"), w.location},
				{F("t"), String(w.temperature)},
				{F("tf"), String(w.temperatureForecast)},
				{F("df"), w.description},
				{F("ws"), String(w.windSpeed)},
				{F("wd"), String(w.windDirection)}
		};
		macroStringReplace(owmStatusPageFS, mapLookup(m), ss);
	}

	logPrintfX(F("WG"), "Sending footer...");
	macroStringReplace(owmFooterPageFS, [](const char*) {return String();}, ss);
	logPrintfX(F("WG"), "Total size: %d", ss.buffer.length());

	webServer.send(200, textHtml, ss.buffer);
}

String WeatherGetter::getWeatherDescription()
{
	String r;
	r.reserve(256);

	for (size_t i = 0; i < weathers.size(); ++i)
	{
		const Weather& w = weathers[i];

		if (w.location.length() == 0)
			continue;

		r += w.location;
		r += " ";
		r += String(w.temperature, 1);
		r += '\x80';
		r += 'C';
		r += " (";
		r += String(w.temperatureForecast, 1);
		r += '\x80';
		r += 'C';
		r += ", ";
		r += w.description;
		r += ") wind: ";
		r += String(w.windSpeed, 1);
		r += " m/s ";
		r += bearing(w.windDirection);
		//		r += " ";
		//		r += w.pressure;
		//		r += " hPa";

		r += " -- ";
	}

	if (r.endsWith(" -- "))
	{
		r.remove(r.length() - 4);	//remove the end
	}

	return r;
}



static RegisterTask r1(new WeatherGetter, TaskDescriptor::CONNECTED | TaskDescriptor::SLOW);


/*
 * Built-in time zones: the world's main cities with their current rules as
 * POSIX TZ strings. Used when no compiled zone file is installed, and for
 * world clocks and time zone pickers. Rules are current as of 2026; past
 * changes are not recorded (only today's and future times matter here).
 */
#include "zr.h"

static const ZoneInfo ZONES[] = {
	{ "UTC", "Coordinated Universal Time", "", "UTC0" },
	/* Europe */
	{ "Europe/London", "London", "United Kingdom", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Dublin", "Dublin", "Ireland", "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Europe/Lisbon", "Lisbon", "Portugal", "WET0WEST,M3.5.0/1,M10.5.0" },
	{ "Atlantic/Reykjavik", "Reykjavík", "Iceland", "GMT0" },
	{ "Atlantic/Azores", "Azores", "Portugal", "<-01>1<+00>,M3.5.0/0,M10.5.0/1" },
	{ "Europe/Paris", "Paris", "France", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Berlin", "Berlin", "Germany", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid", "Madrid", "Spain", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Rome", "Rome", "Italy", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Amsterdam", "Amsterdam", "Netherlands", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Brussels", "Brussels", "Belgium", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Vienna", "Vienna", "Austria", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zurich", "Zurich", "Switzerland", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Stockholm", "Stockholm", "Sweden", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Oslo", "Oslo", "Norway", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Copenhagen", "Copenhagen", "Denmark", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Warsaw", "Warsaw", "Poland", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Prague", "Prague", "Czechia", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Budapest", "Budapest", "Hungary", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Belgrade", "Belgrade", "Serbia", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Zagreb", "Zagreb", "Croatia", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Athens", "Athens", "Greece", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Bucharest", "Bucharest", "Romania", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Sofia", "Sofia", "Bulgaria", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Helsinki", "Helsinki", "Finland", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Kyiv", "Kyiv", "Ukraine", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Riga", "Riga", "Latvia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Vilnius", "Vilnius", "Lithuania", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Tallinn", "Tallinn", "Estonia", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Europe/Chisinau", "Chișinău", "Moldova", "EET-2EEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Istanbul", "Istanbul", "Türkiye", "<+03>-3" },
	{ "Europe/Minsk", "Minsk", "Belarus", "<+03>-3" },
	{ "Europe/Moscow", "Moscow", "Russia", "MSK-3" },
	/* Africa */
	{ "Africa/Casablanca", "Casablanca", "Morocco", "<+01>-1" },
	{ "Africa/Algiers", "Algiers", "Algeria", "CET-1" },
	{ "Africa/Accra", "Accra", "Ghana", "GMT0" },
	{ "Africa/Lagos", "Lagos", "Nigeria", "WAT-1" },
	{ "Africa/Cairo", "Cairo", "Egypt", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Africa/Nairobi", "Nairobi", "Kenya", "EAT-3" },
	{ "Africa/Addis_Ababa", "Addis Ababa", "Ethiopia", "EAT-3" },
	{ "Africa/Johannesburg", "Johannesburg", "South Africa", "SAST-2" },
	{ "Africa/Kinshasa", "Kinshasa", "DR Congo", "WAT-1" },
	/* Asia */
	{ "Asia/Jerusalem", "Jerusalem", "Israel", "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Asia/Beirut", "Beirut", "Lebanon", "EET-2EEST,M3.5.0/0,M10.5.0/0" },
	{ "Asia/Amman", "Amman", "Jordan", "<+03>-3" },
	{ "Asia/Riyadh", "Riyadh", "Saudi Arabia", "<+03>-3" },
	{ "Asia/Baghdad", "Baghdad", "Iraq", "<+03>-3" },
	{ "Asia/Tehran", "Tehran", "Iran", "<+0330>-3:30" },
	{ "Asia/Dubai", "Dubai", "United Arab Emirates", "<+04>-4" },
	{ "Asia/Baku", "Baku", "Azerbaijan", "<+04>-4" },
	{ "Asia/Tbilisi", "Tbilisi", "Georgia", "<+04>-4" },
	{ "Asia/Yerevan", "Yerevan", "Armenia", "<+04>-4" },
	{ "Asia/Kabul", "Kabul", "Afghanistan", "<+0430>-4:30" },
	{ "Asia/Karachi", "Karachi", "Pakistan", "PKT-5" },
	{ "Asia/Tashkent", "Tashkent", "Uzbekistan", "<+05>-5" },
	{ "Asia/Almaty", "Almaty", "Kazakhstan", "<+05>-5" },
	{ "Asia/Kolkata", "Kolkata", "India", "IST-5:30" },
	{ "Asia/Colombo", "Colombo", "Sri Lanka", "<+0530>-5:30" },
	{ "Asia/Kathmandu", "Kathmandu", "Nepal", "<+0545>-5:45" },
	{ "Asia/Dhaka", "Dhaka", "Bangladesh", "<+06>-6" },
	{ "Asia/Yangon", "Yangon", "Myanmar", "<+0630>-6:30" },
	{ "Asia/Bangkok", "Bangkok", "Thailand", "<+07>-7" },
	{ "Asia/Ho_Chi_Minh", "Ho Chi Minh City", "Vietnam", "<+07>-7" },
	{ "Asia/Jakarta", "Jakarta", "Indonesia", "WIB-7" },
	{ "Asia/Shanghai", "Shanghai", "China", "CST-8" },
	{ "Asia/Hong_Kong", "Hong Kong", "China", "HKT-8" },
	{ "Asia/Taipei", "Taipei", "Taiwan", "CST-8" },
	{ "Asia/Singapore", "Singapore", "Singapore", "<+08>-8" },
	{ "Asia/Kuala_Lumpur", "Kuala Lumpur", "Malaysia", "<+08>-8" },
	{ "Asia/Manila", "Manila", "Philippines", "PST-8" },
	{ "Asia/Seoul", "Seoul", "South Korea", "KST-9" },
	{ "Asia/Tokyo", "Tokyo", "Japan", "JST-9" },
	{ "Asia/Vladivostok", "Vladivostok", "Russia", "<+10>-10" },
	/* Oceania */
	{ "Australia/Perth", "Perth", "Australia", "AWST-8" },
	{ "Australia/Darwin", "Darwin", "Australia", "ACST-9:30" },
	{ "Australia/Adelaide", "Adelaide", "Australia", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Brisbane", "Brisbane", "Australia", "AEST-10" },
	{ "Australia/Sydney", "Sydney", "Australia", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Melbourne", "Melbourne", "Australia", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Hobart", "Hobart", "Australia", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Pacific/Auckland", "Auckland", "New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Pacific/Fiji", "Suva", "Fiji", "<+12>-12" },
	{ "Pacific/Honolulu", "Honolulu", "United States", "HST10" },
	/* Americas */
	{ "America/Anchorage", "Anchorage", "United States", "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "America/Los_Angeles", "Los Angeles", "United States", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Vancouver", "Vancouver", "Canada", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Phoenix", "Phoenix", "United States", "MST7" },
	{ "America/Denver", "Denver", "United States", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Edmonton", "Edmonton", "Canada", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Chicago", "Chicago", "United States", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Winnipeg", "Winnipeg", "Canada", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Mexico_City", "Mexico City", "Mexico", "CST6" },
	{ "America/Guatemala", "Guatemala City", "Guatemala", "CST6" },
	{ "America/New_York", "New York", "United States", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Toronto", "Toronto", "Canada", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Havana", "Havana", "Cuba", "CST5CDT,M3.2.0/0,M11.1.0/1" },
	{ "America/Panama", "Panama City", "Panama", "EST5" },
	{ "America/Bogota", "Bogotá", "Colombia", "<-05>5" },
	{ "America/Lima", "Lima", "Peru", "<-05>5" },
	{ "America/Caracas", "Caracas", "Venezuela", "<-04>4" },
	{ "America/Halifax", "Halifax", "Canada", "AST4ADT,M3.2.0,M11.1.0" },
	{ "America/Puerto_Rico", "San Juan", "Puerto Rico", "AST4" },
	{ "America/Santiago", "Santiago", "Chile", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
	{ "America/St_Johns", "St. John's", "Canada", "NST3:30NDT,M3.2.0,M11.1.0" },
	{ "America/Sao_Paulo", "São Paulo", "Brazil", "<-03>3" },
	{ "America/Argentina/Buenos_Aires", "Buenos Aires", "Argentina", "<-03>3" },
	{ "America/Montevideo", "Montevideo", "Uruguay", "<-03>3" },
};

const ZoneInfo *tz_zones(usize *count)
{
	*count = ARRAY_LEN(ZONES);
	return ZONES;
}

const ZoneInfo *tz_find(const char *zone)
{
	for (usize i = 0; i < ARRAY_LEN(ZONES); i++)
		if (str_eq(ZONES[i].zone, zone))
			return &ZONES[i];
	return NULL;
}

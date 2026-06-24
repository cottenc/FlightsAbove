#include "aircraft.h"
#include "aircraft_store.h"
#include "readsb_json_parser.h"
#include "route_cache.h"

#include <cassert>
#include <string>

int main() {
    adsb::ReadsbJsonParser parser;
    adsb::AircraftUpdate updates[4];

    const char* aircraftJson =
        "{ \"aircraft\" : ["
        "{\"hex\":\"acb46d\",\"flight\":\"ASA328  \",\"alt_baro\":10225,"
        "\"gs\":298.6,\"track\":60.95,\"t\":\"B39M\",\"desc\":\"BOEING 737 MAX 9\","
        "\"category\":\"A3\",\"lat\":47.714833,\"lon\":-122.430443,"
        "\"r_dst\":4.937,\"r_dir\":290.7},"
        "{\"hex\":\"a00002\",\"flight\":\"UAL0007  \",\"alt_geom\":30100,"
        "\"gs\":451.2,\"track\":180.1,\"lat\":47.8,\"lon\":-122.1},"
        "{\"hex\":\"a00003\",\"alt_baro\":\"ground\",\"lat\":47.7,\"lon\":-122.2}"
        "]}";

    const size_t updateCount = parser.parseAircraftJson(aircraftJson, updates, 4);
    assert(updateCount == 3);
    assert(updates[0].icao == "ACB46D");
    assert(updates[0].callsign == "ASA328");
    assert(updates[0].normalizedCallsign == "ASA328");
    assert(updates[0].hasAltitude && updates[0].altitudeFt == 10225);
    assert(updates[0].hasGroundSpeed && updates[0].groundSpeedKt == 299);
    assert(updates[0].hasTrack && updates[0].trackDeg == 61);
    assert(updates[0].hasPosition);
    assert(updates[0].hasDistance && updates[0].distanceNm > 4.9 && updates[0].distanceNm < 5.0);
    assert(updates[0].hasBearing && updates[0].bearingDeg == 291);
    assert(updates[0].hasType && updates[0].typeCode == "B39M");
    assert(updates[0].typeDescription == "BOEING 737 MAX 9");
    assert(updates[0].category == "A3");
    assert(adsb::airlineIcaoFromCallsign(updates[0].normalizedCallsign) == "ASA");
    assert(adsb::airlineIcaoFromCallsign(" UAL0007 ") == "UAL");
    assert(adsb::airlineIcaoFromCallsign("N12345").empty());
    assert(adsb::airlineIcaoFromCallsign("AS328").empty());

    adsb::AircraftStore store(47.68571, -122.31595, 120000);
    updates[0].distanceNm = 598.0;
    updates[0].bearingDeg = 0;
    store.applyUpdate(updates[0], 1000);
    adsb::Aircraft aircraft[1];
    assert(store.snapshot(aircraft, 1, 1000) == 1);
    assert(aircraft[0].hasDistance && aircraft[0].distanceNm > 4.9 && aircraft[0].distanceNm < 5.0);
    assert(aircraft[0].hasBearing && aircraft[0].bearingDeg == 291);

    assert(updates[1].callsign == "UAL0007");
    assert(updates[1].normalizedCallsign == "UAL7");
    assert(updates[2].icao == "A00003");
    assert(!updates[2].hasCallsign);
    assert(!updates[2].hasAltitude);

    const char* routeJson =
        "[{\"_airport_codes_iata\":\"SEA-ORD\",\"callsign\":\"ASA328\",\"plausible\":true}]";
    adsb::RouteResult results[2];
    const size_t routeCount = adsb::parseRouteResultsJson(routeJson, results, 2);
    assert(routeCount == 1);
    assert(results[0].callsign == "ASA328");
    assert(results[0].route == "SEA-ORD");

    adsb::RouteRequest requests[1];
    requests[0].callsign = "ASA328";
    requests[0].latitude = 47.714833;
    requests[0].longitude = -122.430443;
    const std::string request = adsb::buildRouteRequestJson(requests, 1);
    assert(request.find("\"callsign\":\"ASA328\"") != std::string::npos);
    assert(request.find("\"lat\":47.714833") != std::string::npos);
    assert(request.find("\"lng\":-122.430443") != std::string::npos);

    adsb::RouteCache cache;
    std::string route;
    assert(cache.shouldLookup("ASA328", 1000));
    cache.markFound("ASA328", "SEA-ORD", 1000);
    assert(!cache.shouldLookup("ASA328", 2000));
    assert(cache.routeFor("ASA328", 2000, route));
    assert(route == "SEA-ORD");
    assert(cache.shouldLookup("ASA328", 1000 + adsb::RouteCache::kFoundTtlMs));

    cache.markMissing("UAL7", 3000);
    assert(!cache.shouldLookup("UAL7", 3000 + 1000));
    assert(cache.shouldLookup("UAL7", 3000 + adsb::RouteCache::kMissingTtlMs));

    return 0;
}

import CoreLocation
import CoreMotion
import EventKit
import Foundation

/// Builds the lines the pager shows on its home screen: one for the weather, then upcoming events.
final class Feeds: NSObject, ObservableObject, CLLocationManagerDelegate {
    @Published var weatherLine: String = UserDefaults.standard.string(forKey: "weatherLine") ?? ""
    @Published var eventLines: [String] = []
    @Published var showWeather: Bool = UserDefaults.standard.object(forKey: "showWeather") as? Bool ?? true {
        didSet { UserDefaults.standard.set(showWeather, forKey: "showWeather") }
    }
    @Published var showCalendar: Bool = UserDefaults.standard.object(forKey: "showCalendar") as? Bool ?? true {
        didSet { UserDefaults.standard.set(showCalendar, forKey: "showCalendar") }
    }
    @Published var useCelsius: Bool = UserDefaults.standard.bool(forKey: "useCelsius") {
        didSet { UserDefaults.standard.set(useCelsius, forKey: "useCelsius") }
    }
    @Published var showSteps: Bool = UserDefaults.standard.bool(forKey: "showSteps") {
        didSet { UserDefaults.standard.set(showSteps, forKey: "showSteps") }
    }
    @Published var stepsLine: String = ""
    @Published var calendarAllowed = false
    @Published var locationAllowed = false

    private let locator = CLLocationManager()
    private let store = EKEventStore()
    private let pedometer = CMPedometer()

    override init() {
        super.init()
        locator.delegate = self
        locator.desiredAccuracy = kCLLocationAccuracyKilometer
        refreshPermissions()
    }

    func refreshPermissions() {
        let status = locator.authorizationStatus
        locationAllowed = status == .authorizedWhenInUse || status == .authorizedAlways
        calendarAllowed = EKEventStore.authorizationStatus(for: .event) == .fullAccess
    }

    func askForAccess() {
        locator.requestWhenInUseAuthorization()
        store.requestFullAccessToEvents { [weak self] _, _ in
            DispatchQueue.main.async { self?.refreshPermissions() }
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        refreshPermissions()
        if locationAllowed { manager.requestLocation() }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let here = locations.last else { return }
        let defaults = UserDefaults.standard
        defaults.set(here.coordinate.latitude, forKey: "lat")
        defaults.set(here.coordinate.longitude, forKey: "lon")
        defaults.set(max(here.horizontalAccuracy, 0), forKey: "fixAccuracy")
        defaults.set(here.timestamp.timeIntervalSince1970, forKey: "fixAt")
        finishPrecise(here)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        finishPrecise(nil)
    }

    private var preciseWaiters: [(CLLocation?) -> Void] = []

    private func finishPrecise(_ location: CLLocation?) {
        let waiting = preciseWaiters
        preciseWaiters = []
        locator.desiredAccuracy = kCLLocationAccuracyKilometer
        waiting.forEach { $0(location) }
    }

    /// A fresh, accurate position for saving a waypoint. Only works while the app is on screen.
    func preciseFix() async -> CLLocation? {
        guard locationAllowed else { return nil }
        return await withCheckedContinuation { done in
            DispatchQueue.main.async {
                self.preciseWaiters.append { done.resume(returning: $0) }
                self.locator.desiredAccuracy = kCLLocationAccuracyBest
                self.locator.requestLocation()
            }
        }
    }

    /// The last position the phone gave the app: latitude, longitude, accuracy in metres, age in seconds.
    var lastFix: (lat: Double, lon: Double, accuracy: Double, age: Double)? {
        let defaults = UserDefaults.standard
        let at = defaults.double(forKey: "fixAt")
        guard at > 0, defaults.object(forKey: "lat") != nil else { return nil }
        return (defaults.double(forKey: "lat"), defaults.double(forKey: "lon"),
                defaults.double(forKey: "fixAccuracy"), max(0, Date().timeIntervalSince1970 - at))
    }

    /// Updates the saved position (only possible while the app is on screen).
    func updateLocation() {
        if locationAllowed { locator.requestLocation() }
    }

    /// Today's step count from the phone's motion sensor, or nil when it is unavailable or not allowed.
    private func stepsToday() async -> String? {
        guard CMPedometer.isStepCountingAvailable() else { return nil }
        let start = Calendar.current.startOfDay(for: Date())
        return await withCheckedContinuation { done in
            pedometer.queryPedometerData(from: start, to: Date()) { data, _ in
                guard let steps = data?.numberOfSteps.intValue else { done.resume(returning: nil); return }
                let number = NumberFormatter.localizedString(from: NSNumber(value: steps), number: .decimal)
                done.resume(returning: "\(number) steps today")
            }
        }
    }

    /// Gathers everything and returns four slots to send: weather, two events, steps.
    /// A slot that is switched off or unavailable is an empty string.
    func gather() async -> [String] {
        var weather = ""
        if showWeather {
            if let fresh = await fetchWeather() {
                await MainActor.run {
                    weatherLine = fresh
                    UserDefaults.standard.set(fresh, forKey: "weatherLine")
                }
                weather = fresh
            } else {
                weather = weatherLine
            }
        }
        var events: [String] = []
        if showCalendar {
            events = upcomingEvents()
            let found = events
            await MainActor.run { eventLines = found }
        }
        var steps = ""
        if showSteps, let line = await stepsToday() { steps = line }
        let stepsNow = steps
        await MainActor.run { stepsLine = stepsNow }
        return [weather, events.first ?? "", events.dropFirst().first ?? "", steps]
    }

    private func fetchWeather() async -> String? {
        let defaults = UserDefaults.standard
        guard defaults.object(forKey: "lat") != nil else { return nil }
        let lat = defaults.double(forKey: "lat"), lon = defaults.double(forKey: "lon")
        let unit = useCelsius ? "celsius" : "fahrenheit"
        let address = "https://api.open-meteo.com/v1/forecast?latitude=\(lat)&longitude=\(lon)"
            + "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
            + "&temperature_unit=\(unit)&timezone=auto&forecast_days=1"
        guard let url = URL(string: address) else { return nil }
        var request = URLRequest(url: url)
        request.timeoutInterval = 8
        guard let (data, _) = try? await URLSession.shared.data(for: request),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let current = json["current"] as? [String: Any],
              let temp = current["temperature_2m"] as? Double else { return nil }
        let code = current["weather_code"] as? Int ?? -1
        let letter = useCelsius ? "C" : "F"
        var line = "\(Int(temp.rounded()))\(letter) \(Self.sky(code))"
        if let daily = json["daily"] as? [String: Any],
           let high = (daily["temperature_2m_max"] as? [Double])?.first,
           let low = (daily["temperature_2m_min"] as? [Double])?.first {
            line += "  H\(Int(high.rounded())) L\(Int(low.rounded()))"
            if let rain = (daily["precipitation_probability_max"] as? [Double])?.first, rain >= 20 {
                line += "  rain \(Int(rain))%"
            }
        }
        return line
    }

    /// The next two timed events in the coming 24 hours.
    private func upcomingEvents() -> [String] {
        guard EKEventStore.authorizationStatus(for: .event) == .fullAccess else { return [] }
        let now = Date()
        let predicate = store.predicateForEvents(withStart: now, end: now.addingTimeInterval(24 * 3600), calendars: nil)
        let timeFormat = DateFormatter()
        timeFormat.dateFormat = "h:mma"
        timeFormat.amSymbol = "a"
        timeFormat.pmSymbol = "p"
        return store.events(matching: predicate)
            .filter { !$0.isAllDay && $0.endDate > now }
            .sorted { $0.startDate < $1.startDate }
            .prefix(2)
            .map { event in
                let day = Calendar.current.isDateInToday(event.startDate) ? "" : "Tmrw "
                return "\(day)\(timeFormat.string(from: event.startDate)) \(event.title ?? "Event")"
            }
    }

    static func sky(_ code: Int) -> String {
        switch code {
        case 0: return "Clear"
        case 1, 2: return "Partly cloudy"
        case 3: return "Cloudy"
        case 45, 48: return "Fog"
        case 51, 53, 55, 56, 57: return "Drizzle"
        case 61, 63, 65, 66, 67, 80, 81, 82: return "Rain"
        case 71, 73, 75, 77, 85, 86: return "Snow"
        case 95, 96, 99: return "Storms"
        default: return ""
        }
    }
}

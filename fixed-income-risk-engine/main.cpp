#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <filesystem>

struct Bond {
    std::string cusip, issuer, sector, rating;
    double coupon, maturity_years, ytm, face_value;
    int position;
};

struct Scenario {
    std::string name, description;
    double rate_shock_bps, spread_shock_bps;
};

double priceAtYTM(const Bond& b, double ytm_pct) {
    int    n = std::max(1, (int)std::round(b.maturity_years * 2));
    double c = b.coupon / 2.0;
    double y = ytm_pct / 2.0 / 100.0;
    double pv = 0.0;
    for (int t = 1; t <= n; ++t)
        pv += c / std::pow(1.0 + y, t);
    pv += 100.0 / std::pow(1.0 + y, n);
    return pv;
}

double cleanPrice(const Bond& b) { return priceAtYTM(b, b.ytm); }
double marketValue(const Bond& b) { return cleanPrice(b) / 100.0 * b.face_value * b.position; }

double modDuration(const Bond& b) {
    double bump = 0.01, p = cleanPrice(b);
    return -(priceAtYTM(b, b.ytm + bump) - priceAtYTM(b, b.ytm - bump))
        / (2.0 * bump * p / 100.0);
}

double convexity(const Bond& b) {
    double bump = 0.01, p = cleanPrice(b);
    return (priceAtYTM(b, b.ytm + bump) - 2.0 * p + priceAtYTM(b, b.ytm - bump))
        / (bump / 100.0 * bump / 100.0 * p);
}

double dv01(const Bond& b) {
    double bump = 0.01;
    return (priceAtYTM(b, b.ytm - bump) - priceAtYTM(b, b.ytm + bump))
        / 2.0 / 100.0 * b.face_value * b.position;
}

std::vector<std::string> splitCSV(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool inQ = false;
    for (char ch : line) {
        if (ch == '"') inQ = !inQ;
        else if (ch == ',' && !inQ) { fields.push_back(field); field.clear(); }
        else field += ch;
    }
    fields.push_back(field);
    return fields;
}

std::vector<Bond> loadPortfolio(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + path);
    std::vector<Bond> bonds;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto v = splitCSV(line);
        if (v.size() < 9) continue;
        Bond b;
        b.cusip = v[0]; b.issuer = v[1]; b.sector = v[2]; b.rating = v[3];
        b.coupon = std::stod(v[4]); b.maturity_years = std::stod(v[5]);
        b.ytm = std::stod(v[6]); b.face_value = std::stod(v[7]); b.position = std::stoi(v[8]);
        bonds.push_back(b);
    }
    return bonds;
}

std::vector<Scenario> loadScenarios(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open: " + path);
    std::vector<Scenario> scenarios;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        for (char& c : line) if ((unsigned char)c > 127) c = '-';
        auto v = splitCSV(line);
        if (v.size() < 4) continue;
        Scenario s;
        s.name = v[0]; s.rate_shock_bps = std::stod(v[1]);
        s.spread_shock_bps = std::stod(v[2]); s.description = v[3];
        scenarios.push_back(s);
    }
    return scenarios;
}

std::string fmt(double v, int prec = 2) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

int main() {
    // ▼ Update these paths before running
    std::string portfolioPath = "path/to/portfolio.csv";
    std::string scenarioPath  = "path/to/scenarios.csv";
    std::string outDir        = "path/to/output";
    // ▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲
    std::filesystem::create_directories(outDir);

    auto bonds = loadPortfolio(portfolioPath);
    auto scenarios = loadScenarios(scenarioPath);
    std::cout << bonds.size() << " bonds, " << scenarios.size() << " scenarios loaded\n";

    double baseMV = 0.0;
    for (const auto& b : bonds) baseMV += marketValue(b);

    // ── 1. base_metrics.csv ───────────────────────────────────────────────────
    {
        std::ofstream f(outDir + "/base_metrics.csv");
        f << "cusip,issuer,sector,rating,coupon,maturity_years,ytm,"
            << "clean_price,market_value,mod_duration,convexity,dv01,cs01\n";
        for (const auto& b : bonds) {
            f << b.cusip << "," << b.issuer << "," << b.sector << "," << b.rating << ","
                << fmt(b.coupon, 3) << "," << fmt(b.maturity_years, 1) << "," << fmt(b.ytm, 3) << ","
                << fmt(cleanPrice(b), 4) << "," << fmt(marketValue(b), 0) << ","
                << fmt(modDuration(b), 4) << "," << fmt(convexity(b), 4) << ","
                << fmt(dv01(b), 2) << "," << fmt(dv01(b), 2) << "\n";
        }
        std::cout << "-> base_metrics.csv\n";
    }

    // ── 2. scenario_results.csv ───────────────────────────────────────────────
    {
        std::ofstream f(outDir + "/scenario_results.csv");
        f << "scenario,rate_shock_bps,spread_shock_bps,portfolio_pnl,portfolio_pnl_pct,description\n";
        for (const auto& sc : scenarios) {
            double shock = sc.rate_shock_bps + sc.spread_shock_bps;
            double pnl = 0.0;
            for (const auto& b : bonds) {
                double newMV = priceAtYTM(b, b.ytm + shock / 100.0)
                    / 100.0 * b.face_value * b.position;
                pnl += newMV - marketValue(b);
            }
            double pct = baseMV > 0 ? pnl / baseMV * 100.0 : 0.0;
            f << sc.name << "," << fmt(sc.rate_shock_bps, 0) << ","
                << fmt(sc.spread_shock_bps, 0) << "," << fmt(pnl, 0) << ","
                << fmt(pct, 4) << "," << sc.description << "\n";
        }
        std::cout << "-> scenario_results.csv\n";
    }

    // ── 3. summary.csv ────────────────────────────────────────────────────────
    {
        std::ofstream f(outDir + "/summary.csv");

        // Portfolio-level stats
        double totalDV01 = 0.0, wtdDur = 0.0, wtdConvex = 0.0;
        for (const auto& b : bonds) {
            double mv = marketValue(b);
            totalDV01 += dv01(b);
            wtdDur += modDuration(b) * mv;
            wtdConvex += convexity(b) * mv;
        }
        double portDur = baseMV > 0 ? wtdDur / baseMV : 0.0;
        double portConvex = baseMV > 0 ? wtdConvex / baseMV : 0.0;

        f << "PORTFOLIO SUMMARY\n";
        f << "metric,value\n";
        f << "Total Bonds," << bonds.size() << "\n";
        f << "Total Market Value," << fmt(baseMV, 0) << "\n";
        f << "Portfolio Duration," << fmt(portDur, 4) << "\n";
        f << "Portfolio Convexity," << fmt(portConvex, 4) << "\n";
        f << "Total DV01," << fmt(totalDV01, 0) << "\n";

        // Sector breakdown
        f << "\nSECTOR EXPOSURE\n";
        f << "sector,market_value,weight_pct\n";
        std::map<std::string, double> sectorMV;
        for (const auto& b : bonds) sectorMV[b.sector] += marketValue(b);
        for (const auto& kv : sectorMV) {
            double pct = baseMV > 0 ? kv.second / baseMV * 100.0 : 0.0;
            f << kv.first << "," << fmt(kv.second, 0) << "," << fmt(pct, 2) << "\n";
        }

        // Top 5 DV01
        f << "\nTOP 5 DV01 CONTRIBUTORS\n";
        f << "issuer,rating,market_value,mod_duration,dv01\n";
        auto sorted = bonds;
        std::sort(sorted.begin(), sorted.end(),
            [](const Bond& a, const Bond& b) { return dv01(a) > dv01(b); });
        for (int i = 0; i < 5 && i < (int)sorted.size(); ++i) {
            const auto& b = sorted[i];
            f << b.issuer << "," << b.rating << ","
                << fmt(marketValue(b), 0) << "," << fmt(modDuration(b), 4) << ","
                << fmt(dv01(b), 2) << "\n";
        }

        std::cout << "-> summary.csv\n";
    }

    std::cout << "\nDone.\n";
    return 0;
}

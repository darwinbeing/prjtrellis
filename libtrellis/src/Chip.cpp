#include "Chip.hpp"
#include "Tile.hpp"
#include "Database.hpp"
#include "Util.hpp"
#include "RoutingGraph.hpp"
#include "BitDatabase.hpp"
#include "Bels.hpp"
#include <algorithm>
#include <iostream>
using namespace std;

namespace Trellis {

Chip::Chip(string name) : Chip(get_chip_info(find_device_by_name(name)))
{}

Chip::Chip(uint32_t idcode) : Chip(get_chip_info(find_device_by_idcode(idcode)))
{}

Chip::Chip(const Trellis::ChipInfo &info) : info(info), cram(info.num_frames, info.bits_per_frame)
{
    vector<TileInfo> allTiles = get_device_tilegrid(DeviceLocator{info.family, info.name});
    for (const auto &tile : allTiles) {
        tiles[tile.name] = make_shared<Tile>(tile, *this);
        int row, col;
        tie(row, col) = tile.get_row_col();
        if (int(tiles_at_location.size()) <= row) {
            tiles_at_location.resize(row+1);
        }
        if (int(tiles_at_location.at(row).size()) <= col) {
            tiles_at_location.at(row).resize(col+1);
        }
        tiles_at_location.at(row).at(col).push_back(make_pair(tile.name, tile.type));
    }
    global_data = get_global_info(DeviceLocator{info.family, info.name});
}

shared_ptr<Tile> Chip::get_tile_by_name(string name)
{
    return tiles.at(name);
}

vector<shared_ptr<Tile>> Chip::get_tiles_by_position(int row, int col)
{
    vector<shared_ptr<Tile>> result;
    for (const auto &tile : tiles) {
        if (tile.second->info.get_row_col() == make_pair(row, col))
            result.push_back(tile.second);
    }
    return result;
}

string Chip::get_tile_by_position_and_type(int row, int col, string type) {
    for (const auto &tile : tiles_at_location.at(row).at(col)) {
        if (tile.second == type)
            return tile.first;
    }
    throw runtime_error(fmt("no suitable tile found at R" << row << "C" << col));
}

string Chip::get_tile_by_position_and_type(int row, int col, set<string> type) {
    for (const auto &tile : tiles_at_location.at(row).at(col)) {
        if (type.find(tile.second) != type.end())
            return tile.first;
    }
    throw runtime_error(fmt("no suitable tile found at R" << row << "C" << col));
}


vector<shared_ptr<Tile>> Chip::get_tiles_by_type(string type)
{
    vector<shared_ptr<Tile>> result;
    for (const auto &tile : tiles) {
        if (tile.second->info.type == type)
            result.push_back(tile.second);
    }
    return result;
}

vector<shared_ptr<Tile>> Chip::get_all_tiles()
{
    vector<shared_ptr<Tile>> result;
    for (const auto &tile : tiles) {
        result.push_back(tile.second);
    }
    return result;
}

int Chip::get_max_row() const
{
    return max_element(tiles.begin(), tiles.end(),
                       [](const decltype(tiles)::value_type &a, const decltype(tiles)::value_type &b) {
                           return a.second->info.get_row_col().first < b.second->info.get_row_col().first;
                       })->second->info.get_row_col().first;
}

int Chip::get_max_col() const
{
    return max_element(tiles.begin(), tiles.end(),
                       [](const decltype(tiles)::value_type &a, const decltype(tiles)::value_type &b) {
                           return a.second->info.get_row_col().second < b.second->info.get_row_col().second;
                       })->second->info.get_row_col().second;
}

ChipDelta operator-(const Chip &a, const Chip &b)
{
    ChipDelta delta;
    for (const auto &tile : a.tiles) {
        CRAMDelta cd = tile.second->cram - b.tiles.at(tile.first)->cram;
        if (!cd.empty())
            delta[tile.first] = cd;
    }
    return delta;
}

shared_ptr<RoutingGraph> Chip::get_routing_graph()
{
    shared_ptr<RoutingGraph> rg(new RoutingGraph(*this));
    //cout << "Building routing graph" << endl;
    for (auto tile_entry : tiles) {
        shared_ptr<Tile> tile = tile_entry.second;
        //cout << "    Tile " << tile->info.name << endl;
        shared_ptr<TileBitDatabase> bitdb = get_tile_bitdata(TileLocator{info.family, info.name, tile->info.type});
        bitdb->add_routing(tile->info, *rg);
        int x, y;
        tie(y, x) = tile->info.get_row_col();
        if (tile->info.type == "PLC2") {
            for (int z = 0; z < 4; z++)
                Bels::add_lc(*rg, x, y, z);
        }
        if (tile->info.type.find("PICL0") != string::npos || tile->info.type.find("PICR0") != string::npos)
            for (int z = 0; z < 4; z++)
                Bels::add_pio(*rg, x, y, z);
        if (tile->info.type.find("PIOT0") != string::npos || (tile->info.type.find("PICB0") != string::npos && tile->info.type != "SPICB0"))
            for (int z = 0; z < 2; z++)
                Bels::add_pio(*rg, x, y, z);
    }
    return rg;
}

// Global network funcs

bool GlobalRegion::matches(int row, int col) const {
    return (row >= y0 && row <= y1 && col >= x0 && col <= x1);
}

bool TapSegment::matches_left(int row, int col) const {
    UNUSED(row);
    return (col >= lx0 && col <= lx1);
}

bool TapSegment::matches_right(int row, int col) const {
    UNUSED(row);
    return (col >= rx0 && col <= rx1);
}

string GlobalsInfo::get_quadrant(int row, int col) const {
    for (const auto &quad : quadrants) {
        if (quad.matches(row, col))
            return quad.name;
    }
    throw runtime_error(fmt("R" << row << "C" << col << " matches no globals quadrant"));
}

TapDriver GlobalsInfo::get_tap_driver(int row, int col) const {
    for (const auto &seg : tapsegs) {
        if (seg.matches_left(row, col)) {
            TapDriver td;
            td.dir = TapDriver::LEFT;
            td.col = seg.tap_col;
            return td;
        }
        if (seg.matches_right(row, col)) {
            TapDriver td;
            td.dir = TapDriver::RIGHT;
            td.col = seg.tap_col;
            return td;
        }
    }
    throw runtime_error(fmt("R" << row << "C" << col << " matches no global TAP_DRIVE segment"));
}

}

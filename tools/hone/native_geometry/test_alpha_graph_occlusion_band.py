"""Run the existing pixel/band failure suite with only the collector opted in."""
import pathlib
here=pathlib.Path(__file__).resolve().parent
source=(here/'test_alpha_graph_band.py').read_text()
anchor='#include "tools/hone/native_geometry/alpha_graph_band_candidate.h"'
assert source.count(anchor)==1
source=source.replace(anchor,'#include "tools/hone/native_geometry/alpha_graph_occlusion_band_candidate.h"')
source=source.replace('hone_graph_band_preflight(', 'hone_graph_occlusion_band_preflight(').replace('hone_graph_band_render(', 'hone_graph_occlusion_band_render(')

exec(compile(source,str(here/'test_alpha_graph_band.py'),'exec'))

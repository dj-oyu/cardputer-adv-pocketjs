"""Run the existing pixel/band failure suite with only the collector opted in."""
import pathlib
here=pathlib.Path(__file__).resolve().parent
source=(here/'test_alpha_graph_band.py').read_text()
anchor='#include "tools/hone/native_geometry/alpha_graph_band_candidate.h"'
assert source.count(anchor)==1
source=source.replace(anchor,'#include "tools/hone/native_geometry/alpha_graph_occlusion_candidate.h"\n#define hone_graph_fragments hone_graph_fragments_occlusion\n'+anchor+'\n#undef hone_graph_fragments')
exec(compile(source,str(here/'test_alpha_graph_band.py'),'exec'))

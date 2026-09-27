"""pets-compact.bin as the C array tools/hostshim/ksn_pet_builtin.c includes."""
import pathlib, sys
data = pathlib.Path(sys.argv[1]).read_bytes()
pathlib.Path(sys.argv[2]).write_text(
    'static const uint8_t pet_test_data[] = {' + ','.join(map(str, data)) + '};\n')

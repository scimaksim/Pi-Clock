# eclgen

Generates `renderer/eclipse_data.c`: Besselian elements for every solar
eclipse and shadow elements for every lunar eclipse from 2024 to 2040,
computed from JPL DE421 with Skyfield and fitted as polynomials in UT hours.

    pip install skyfield numpy
    python3 gen.py      # -> elements.json (needs de421.bsp; edit the path in gen.py)
    python3 check.py    # compares against published circumstances
    python3 emit.py     # -> ../../renderer/eclipse_data.c

To extend the range, change `t1`/`t2` in gen.py (DE421 covers 1900-2050).

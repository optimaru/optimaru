import io
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from bench.data import parse_alb_data, parse_xlsx_best_known


def test_salbp_parser_maps_tasks_cycle_time_and_precedence() -> None:
    problem = parse_alb_data(
        """
        <number of tasks>
        3
        <cycle time>
        10
        <order strength>
        0.5
        <task times>
        1 4
        2 5
        3 6
        <precedence relations>
        1,3
        2,3
        <end>
        """
    )

    assert problem.cycle_time == 10
    assert [(task.id, task.duration) for task in problem.tasks] == [("1", 4), ("2", 5), ("3", 6)]
    assert [(edge.before, edge.after) for edge in problem.precedences] == [("1", "3"), ("2", "3")]


def test_salbp_workbook_parser_uses_best_feasible_upper_bound() -> None:
    workbook = io.BytesIO()
    with zipfile.ZipFile(workbook, "w") as archive:
        archive.writestr(
            "xl/sharedStrings.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
              <si><t>instance_n=100_1</t></si><si><t>[20,21]</t></si>
              <si><t>&lt;Filename&gt;</t></si><si><t>&lt;Upper bound on the number of stations&gt;</t></si>
            </sst>""",
        )
        archive.writestr(
            "xl/worksheets/sheet1.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
              <row r="1"/>
              <row r="2"><c r="B2" t="s"><v>2</v></c><c r="I2" t="s"><v>3</v></c></row>
              <row r="3"><c r="B3" t="s"><v>0</v></c><c r="G3" t="s"><v>1</v></c><c r="I3"><v>21</v></c></row>
            </sheetData></worksheet>""",
        )

    assert parse_xlsx_best_known(workbook.getvalue()) == {"instance_n=100_1": 21}


def test_salbp_workbook_without_its_header_row_is_refused() -> None:
    workbook = io.BytesIO()
    with zipfile.ZipFile(workbook, "w") as archive:
        archive.writestr(
            "xl/sharedStrings.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
              <si><t>instance_n=100_1</t></si>
            </sst>""",
        )
        archive.writestr(
            "xl/worksheets/sheet1.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
              <row r="1"/><row r="2"/>
              <row r="3"><c r="B3" t="s"><v>0</v></c><c r="I3"><v>21</v></c></row>
            </sheetData></worksheet>""",
        )

    with pytest.raises(ValueError, match="no '<filename>' column"):
        parse_xlsx_best_known(workbook.getvalue())


def test_salbp_workbook_parser_finds_the_columns_by_their_headers() -> None:
    # The medium workbook has a permutation column before the file name.
    workbook = io.BytesIO()
    with zipfile.ZipFile(workbook, "w") as archive:
        archive.writestr(
            "xl/sharedStrings.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
              <si><t>&lt;Permu-tation No&gt;</t></si><si><t>&lt;Filename&gt;</t></si>
              <si><t>&lt;Upper bound on the number of stations&gt;</t></si>
              <si><t>instance_n=50_1p2</t></si>
            </sst>""",
        )
        archive.writestr(
            "xl/worksheets/sheet1.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
            <worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
              <row r="1"/>
              <row r="2"><c r="B2" t="s"><v>0</v></c><c r="C2" t="s"><v>1</v></c><c r="J2" t="s"><v>2</v></c></row>
              <row r="3"><c r="B3"><v>2</v></c><c r="C3" t="s"><v>3</v></c><c r="I3"><v>7</v></c><c r="J3"><v>8</v></c></row>
            </sheetData></worksheet>""",
        )

    assert parse_xlsx_best_known(workbook.getvalue()) == {"instance_n=50_1p2": 8}

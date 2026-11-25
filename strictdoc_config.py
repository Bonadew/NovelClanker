# StrictDoc project configuration (https://strictdoc.readthedocs.io).
#
# Requirements live in docs/*.sdoc. Source and test files point back at them with
# "@relation(<UID>, scope=...)" markers, which StrictDoc turns into requirement <-> source traceability.
#
#   strictdoc export . --output-dir build/strictdoc   (static HTML: build/strictdoc/html/index.html)
#   strictdoc server .                                (browse and edit in the web UI)
from strictdoc.core.project_config import ProjectConfig


def create_config() -> ProjectConfig:
    return ProjectConfig(
        project_title="NovelClanker",
        project_features=[
            "TABLE_SCREEN",
            "TRACEABILITY_SCREEN",
            "DEEP_TRACEABILITY_SCREEN",
            "SEARCH",
            "TRACEABILITY_MATRIX_SCREEN",
            "REQUIREMENT_TO_SOURCE_TRACEABILITY",
        ],
        include_doc_paths=[
            "/docs/**",
        ],
        # Only .sdoc files are requirements; other Markdown notes in docs/ are not.
        exclude_doc_paths=[
            "/docs/**.md",
        ],
        include_source_paths=[
            "/src/**",
            "/tests/**",
        ],
    )

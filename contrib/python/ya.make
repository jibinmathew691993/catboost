

RECURSE(
    appdirs
    dateutil
    enum34
    Jinja2
    MarkupSafe
    numpy
    packaging
    pandas
    pandas/matplotlib
    py
    pyparsing
    pytest
    pytz
    pytz/tests
    setuptools
    six
    subprocess32
)

IF (OS_DARWIN)
    RECURSE(
    
)
ENDIF ()

IF (OS_LINUX)
    RECURSE(
    
)
ENDIF ()

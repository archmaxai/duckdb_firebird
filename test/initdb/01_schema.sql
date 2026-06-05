-- Seed schema for the DuckDB Firebird extension test database.
-- Exercises a broad range of Firebird column types AND relational structure
-- (foreign keys across DEPARTMENTS / EMPLOYEES / PROJECTS) so that joins,
-- aggregates, subqueries, window functions, set operations, etc. can be
-- validated end-to-end through the catalog extension.

SET SQL DIALECT 3;

CREATE TABLE DEPARTMENTS (
    ID          INTEGER NOT NULL PRIMARY KEY,
    NAME        VARCHAR(30) NOT NULL,
    BUDGET      NUMERIC(14,2),
    MANAGER_ID  INTEGER
);

CREATE TABLE EMPLOYEES (
    ID          INTEGER NOT NULL PRIMARY KEY,
    FIRST_NAME  VARCHAR(50),
    LAST_NAME   VARCHAR(50),
    DEPARTMENT  VARCHAR(30),
    DEPT_ID     INTEGER,
    SALARY      NUMERIC(12,2),
    HIRED_ON    DATE,
    ACTIVE      BOOLEAN
);

CREATE TABLE PROJECTS (
    ID          INTEGER NOT NULL PRIMARY KEY,
    NAME        VARCHAR(60) NOT NULL,
    DEPT_ID     INTEGER,
    LEAD_ID     INTEGER,
    HOURS       INTEGER
);

CREATE TABLE TYPE_GALLERY (
    ID           INTEGER NOT NULL PRIMARY KEY,
    C_SMALLINT   SMALLINT,
    C_INTEGER    INTEGER,
    C_BIGINT     BIGINT,
    C_FLOAT      FLOAT,
    C_DOUBLE     DOUBLE PRECISION,
    C_NUMERIC    NUMERIC(15,4),
    C_DECIMAL    DECIMAL(10,2),
    C_CHAR       CHAR(10),
    C_VARCHAR    VARCHAR(100),
    C_DATE       DATE,
    C_TIME       TIME,
    C_TIMESTAMP  TIMESTAMP,
    C_BOOLEAN    BOOLEAN,
    C_BLOB_TEXT  BLOB SUB_TYPE TEXT,
    C_BLOB_BIN   BLOB SUB_TYPE BINARY
);

COMMIT;

-- Departments. Operations (id 3) deliberately has no manager (NULL) to test
-- outer joins / NULL handling.
INSERT INTO DEPARTMENTS (ID, NAME, BUDGET, MANAGER_ID) VALUES (1, 'Engineering', 500000.00, 1);
INSERT INTO DEPARTMENTS (ID, NAME, BUDGET, MANAGER_ID) VALUES (2, 'Research',    750000.00, 2);
INSERT INTO DEPARTMENTS (ID, NAME, BUDGET, MANAGER_ID) VALUES (3, 'Operations',  300000.00, NULL);

-- Employees. Katherine (id 4) has no department (NULL DEPT_ID) on purpose.
INSERT INTO EMPLOYEES (ID, FIRST_NAME, LAST_NAME, DEPARTMENT, DEPT_ID, SALARY, HIRED_ON, ACTIVE)
    VALUES (1, 'Ada',       'Lovelace', 'Engineering', 1,    95000.00,  DATE '2019-03-01', TRUE);
INSERT INTO EMPLOYEES (ID, FIRST_NAME, LAST_NAME, DEPARTMENT, DEPT_ID, SALARY, HIRED_ON, ACTIVE)
    VALUES (2, 'Alan',      'Turing',   'Research',    2,    105000.50, DATE '2018-07-15', TRUE);
INSERT INTO EMPLOYEES (ID, FIRST_NAME, LAST_NAME, DEPARTMENT, DEPT_ID, SALARY, HIRED_ON, ACTIVE)
    VALUES (3, 'Grace',     'Hopper',   'Engineering', 1,    99000.25,  DATE '2020-01-20', FALSE);
INSERT INTO EMPLOYEES (ID, FIRST_NAME, LAST_NAME, DEPARTMENT, DEPT_ID, SALARY, HIRED_ON, ACTIVE)
    VALUES (4, 'Katherine', 'Johnson',  NULL,          NULL, NULL,      NULL,              NULL);

-- Projects. Project 4 belongs to Operations (dept 3) led by Katherine (id 4).
INSERT INTO PROJECTS (ID, NAME, DEPT_ID, LEAD_ID, HOURS) VALUES (1, 'Analytical Engine', 1, 1, 1200);
INSERT INTO PROJECTS (ID, NAME, DEPT_ID, LEAD_ID, HOURS) VALUES (2, 'Enigma',            2, 2,  800);
INSERT INTO PROJECTS (ID, NAME, DEPT_ID, LEAD_ID, HOURS) VALUES (3, 'COBOL',             1, 3, 1500);
INSERT INTO PROJECTS (ID, NAME, DEPT_ID, LEAD_ID, HOURS) VALUES (4, 'Orbital Mechanics', 3, 4,  600);

INSERT INTO TYPE_GALLERY (ID, C_SMALLINT, C_INTEGER, C_BIGINT, C_FLOAT, C_DOUBLE,
                          C_NUMERIC, C_DECIMAL, C_CHAR, C_VARCHAR, C_DATE, C_TIME,
                          C_TIMESTAMP, C_BOOLEAN, C_BLOB_TEXT, C_BLOB_BIN)
    VALUES (1, 32000, 2000000000, 9000000000000, 3.14, 2.718281828,
            12345.6789, 99.95, 'fixed', 'a varchar value',
            DATE '2021-06-15', TIME '13:45:30', TIMESTAMP '2021-06-15 13:45:30.1234',
            TRUE, 'some long text in a text blob', NULL);

INSERT INTO TYPE_GALLERY (ID, C_SMALLINT, C_INTEGER, C_BIGINT, C_FLOAT, C_DOUBLE,
                          C_NUMERIC, C_DECIMAL, C_CHAR, C_VARCHAR, C_DATE, C_TIME,
                          C_TIMESTAMP, C_BOOLEAN, C_BLOB_TEXT, C_BLOB_BIN)
    VALUES (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL);

COMMIT;

.open test_mult_tables_2025_06_07.db
DROP TABLE IF EXISTS sample_table_1;
DROP TABLE IF EXISTS sample_table_2;
CREATE TABLE sample_table_1 (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
CREATE TABLE sample_table_2 (id INTEGER PRIMARY KEY, name TEXT, age INTEGER);
INSERT INTO sample_table_1 (name, age) VALUES ('This was put in table 1', 90);
INSERT INTO sample_table_2 (name, age) VALUES ('This was put in table 2', 50);
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_1;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_2;
BEGIN TRANSACTION;
INSERT INTO sample_table_2 (name, age) VALUES ('Entry in Table 2 - 1', 150);
INSERT INTO sample_table_2 (name, age) VALUES ('Entry in Table 2 - 2', 250);
INSERT INTO sample_table_2 (name, age) VALUES ('Entry in Table 2 - 3', 350);
INSERT INTO sample_table_2 (name, age) VALUES ('Entry in Table 2 - 4', 450);
INSERT INTO sample_table_1 (name, age) VALUES ('Entry in Table 1 - 1', 850);
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_1;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_2;
SAVEPOINT test_1;
DELETE FROM sample_table_2 where age < 350;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_2;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_1;
ROLLBACK TO test_1;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_1;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_2;
ROLLBACK;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_1;
-- .SQL2C_select (text, 0, name), (int, 1, age)
Select name, age from sample_table_2;

BEGIN;

DELETE FROM ne_policies WHERE profile_id = 30;
DELETE FROM ne_lan WHERE profile_id = 30;
DELETE FROM ne_wan WHERE profile_id = 30;
DELETE FROM ne_profiles WHERE id = 30;

INSERT INTO ne_profiles (id, name, description, weight_enable, latency_enable, loss_enable, created_by)
VALUES (30, 'ip_ssh_l3', 'L3 SSH NE2 peer', FALSE, FALSE, FALSE, 'seed');

INSERT INTO ne_policies (
    id, profile_id, priority, action, proto,
    src_ip, invert_src_ip, dst_ip, invert_dst_ip,
    src_port, dst_port, method, nonce, encryption_key, created_by
) VALUES
(
    22, 30, 1, 'L3', 'tcp',
    ARRAY['192.168.9.2/32']::text[], FALSE,
    ARRAY['192.168.180.2/32']::text[], FALSE,
    ARRAY['ANY']::text[], ARRAY['22']::text[],
    'aes-gcm-128', 12, '00112233445566778899aabbccddeeff', 'seed'
),
(
    23, 30, 2, 'L3', 'tcp',
    ARRAY['192.168.180.0/24']::text[], FALSE,
    ARRAY['192.168.9.0/24']::text[], FALSE,
    ARRAY['22']::text[], ARRAY['ANY']::text[],
    'aes-gcm-128', 12, '00112233445566778899aabbccddeeff', 'seed'
),
(
    99, 30, 3, 'bypass', NULL,
    ARRAY['0.0.0.0/0']::text[], FALSE,
    ARRAY['0.0.0.0/0']::text[], FALSE,
    ARRAY['ANY']::text[], ARRAY['ANY']::text[],
    NULL, NULL, NULL, 'seed'
);

INSERT INTO ne_lan (interface, profile_id, created_by) VALUES
    ('eno2', 30, 'seed'),
    ('enp7s0', 30, 'seed');

INSERT INTO ne_wan (interface, profile_id, dst_ip, weight, created_by) VALUES
    ('enp4s0', 30, '192.168.11.1', 70, 'seed'),
    ('enp5s0', 30, '192.168.131.1', 30, 'seed');

SELECT setval(pg_get_serial_sequence('ne_profiles', 'id')::regclass,
    COALESCE((SELECT MAX(id) FROM ne_profiles), 1), true);

COMMIT;

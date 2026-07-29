#!/usr/bin/env python3
"""Deterministic reg_buh data generator. seed=42, 10M rows."""
import random, sys
random.seed(42)
N = 10_000_000
print("period_key,company_key,account_key,debit_key,credit_key,amount_dt,amount_kt,payload")
for i in range(N):
    pk = (i // 83334) + 1
    ck = (i % 50) + 1
    ak = (i % 200) + 1
    dk = random.randint(1, 100)
    crk = random.randint(1, 100)
    adt = random.randint(1, 100000)
    akt = random.randint(1, 100000)
    pay = random.randint(1, 1000000)
    print(f"{pk},{ck},{ak},{dk},{crk},{adt},{akt},{pay}")

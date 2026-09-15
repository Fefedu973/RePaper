import {
  sqliteTable,
  text,
  integer,
  index,
  uniqueIndex,
} from 'drizzle-orm/sqlite-core';

export const sessions = sqliteTable(
  'sessions',
  {
    id: text('id').primaryKey(),
    code: text('code').notNull(),
    deviceHash: text('device_hash').notNull(),
    moodleBase: text('moodle_base').notNull(),
    passport: text('passport').notNull(),
    siteId: text('site_id').notNull(),
    publicKey: text('public_key').notNull(),
    expiresAt: integer('expires_at').notNull(),
    browserHash: text('browser_hash'),
    ciphertext: text('ciphertext'),
    collected: integer('collected').notNull().default(0),
  },
  (table) => [
    uniqueIndex('sessions_code').on(table.code),
    index('sessions_expiry').on(table.expiresAt),
    uniqueIndex('sessions_browser').on(table.browserHash),
  ],
);

export const rates = sqliteTable(
  'rates',
  {
    key: text('key').primaryKey(),
    hits: integer('hits').notNull(),
    expiresAt: integer('expires_at').notNull(),
  },
  (table) => [index('rates_expiry').on(table.expiresAt)],
);

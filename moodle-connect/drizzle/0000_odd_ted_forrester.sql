CREATE TABLE `rates` (
	`key` text PRIMARY KEY NOT NULL,
	`hits` integer NOT NULL,
	`expires_at` integer NOT NULL
);
--> statement-breakpoint
CREATE INDEX `rates_expiry` ON `rates` (`expires_at`);--> statement-breakpoint
CREATE TABLE `sessions` (
	`id` text PRIMARY KEY NOT NULL,
	`code` text NOT NULL,
	`device_hash` text NOT NULL,
	`moodle_base` text NOT NULL,
	`passport` text NOT NULL,
	`site_id` text NOT NULL,
	`public_key` text NOT NULL,
	`expires_at` integer NOT NULL,
	`browser_hash` text,
	`ciphertext` text,
	`collected` integer DEFAULT 0 NOT NULL
);
--> statement-breakpoint
CREATE UNIQUE INDEX `sessions_code` ON `sessions` (`code`);--> statement-breakpoint
CREATE INDEX `sessions_expiry` ON `sessions` (`expires_at`);--> statement-breakpoint
CREATE UNIQUE INDEX `sessions_browser` ON `sessions` (`browser_hash`);
# Canonical file identities for copying, install checks, managed updates and rollback.
    $InstallerTargets = @(
        [ordered]@{
            Key = 'STARTUP'
            RepositorySource = 'deploy/smartphone_integrator/carplay_startup.sh'
            Source = 'payload/hooks/carplay_startup.sh'
            Destination = '/mnt/app/root/hooks/carplay_startup.sh'
            Mode = '755'
            Action = 'add'
        }
        [ordered]@{
            Key = 'CLEANUP'
            RepositorySource = 'deploy/smartphone_integrator/carplay_cleanup.sh'
            Source = 'payload/hooks/carplay_cleanup.sh'
            Destination = '/mnt/app/root/hooks/carplay_cleanup.sh'
            Mode = '755'
            Action = 'add'
        }
        [ordered]@{
            Key = 'PROCESSES'
            RepositorySource = 'deploy/smartphone_integrator/carplay_processes.sh'
            Source = 'payload/hooks/carplay_processes.sh'
            Destination = '/mnt/app/root/hooks/carplay_processes.sh'
            Mode = '755'
            Action = 'add'
        }
        [ordered]@{
            Key = 'MONITOR'
            RepositorySource = 'deploy/smartphone_integrator/carplay_monitor.sh'
            AllowMissingPreviousState = $true
            Source = 'payload/hooks/carplay_monitor.sh'
            Destination = '/mnt/app/root/hooks/carplay_monitor.sh'
            Mode = '755'
            Action = 'add'
        }
        [ordered]@{
            Key = 'HOOK'
            RepositorySource = 'build/libcarplay_hook.so'
            Source = 'payload/hooks/libcarplay_hook.so'
            Destination = '/mnt/app/root/hooks/libcarplay_hook.so'
            Mode = '644'
            Action = 'add'
        }
        [ordered]@{
            Key = 'RENDERER'
            RepositorySource = 'build/maneuver_render'
            Source = 'payload/hooks/maneuver_render'
            Destination = '/mnt/app/root/hooks/maneuver_render'
            Mode = '755'
            Action = 'add'
        }
        [ordered]@{
            Key = 'ATLAS'
            RepositorySource = 'maneuver_render/resources/flag_atlas.rgba'
            Source = 'payload/hooks/flag_atlas.rgba'
            Destination = '/mnt/app/root/hooks/flag_atlas.rgba'
            Mode = '644'
            Action = 'add'
        }
        [ordered]@{
            Key = 'JAR'
            Source = 'payload/jars/carplay_hook.jar'
            Destination = '/mnt/app/eso/hmi/lsd/jars/carplay_hook.jar'
            Mode = '644'
            Action = 'add'
        }
        [ordered]@{
            Key = 'SMARTPHONE'
            Source = 'payload/config/smartphone_integrator.json'
            Destination = '/mnt/system/etc/eso/production/smartphone_integrator.json'
            # Preserve each unit's existing configuration permissions.
            Mode = 'preserve'
            Action = 'replace-verified-stock'
            RollbackSource = 'rollback/smartphone_integrator.stock.json'
        }
        [ordered]@{
            Key = 'DIO'
            Source = 'payload/config/dio_manager.json'
            Destination = '/mnt/system/etc/eso/production/dio_manager.json'
            Mode = 'preserve'
            Action = 'replace-verified-stock'
            RollbackSource = 'rollback/dio_manager.stock.json'
        }
    )
